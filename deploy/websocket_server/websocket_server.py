import asyncio
import websockets
import logging
import os
import json
import random
import aiohttp

from aiomcache import Client
from functools import partial
from datetime import datetime, timezone, timedelta

debug_level = os.environ.get("DEBUG_LEVEL") or "DEBUG"
websocket_port = os.environ.get("WEBSOCKET_PORT") or 39447
ping_interval = int(os.environ.get("PING_INTERVAL", 20))
memcache_fetch_interval = int(os.environ.get("MEMCACHE_FETCH_INTERVAL", 1))
environment = os.environ.get("ENVIRONMENT") or "TEST"
endpoint_url = os.environ.get("ENDPOINT_URL") or "http://localhost"

logging.basicConfig(level=debug_level, format="%(asctime)s %(levelname)s : %(message)s")
logger = logging.getLogger(__name__)


memcached_host = os.environ.get("MEMCACHED_HOST") or "localhost"
mc = Client(memcached_host, 11211)

nodes_list = [
    "t4a_a",
    "t4a_b",
    "t4a_c",
    "tr1_a",
    "tr1_b",
    "tr1_c",
    "tr3_a",
    "tr3_b",
    "tr3_c",
    "rk2a_a",
    "rk2a_b",
    "rk2a_c",
    "tr5",
    "rk4",
    "rk6",
    "tr4_a",
    "tr4_b",
    "bsh7",
    "bsh9",
    "bsh10_8_a",
    "bsh10_8_b",
    "pr1",
    "pr2"
]


empty_node = {
    "grid": "unknown",
    "grid_change_time": 0,
    "status_change_time": 0
}


class SharedData:
    def __init__(self):
        self.clients = {}
        self.wifi_firmware_url = ""
        self.eth_firmware_url = ""
        self.firmware_version = ""


shared_data = SharedData()


async def loop_data(websocket, shared_data: SharedData):
    client_ip, client_port = websocket.remote_address

    while True:
        client = shared_data.clients[f"{client_ip}_{client_port}"]
        client_node = client["node"]
        client_connect_mode = client["connect_mode"]
        current_firmware = shared_data.firmware_version
        current_firmware_wifi_url = shared_data.wifi_firmware_url
        current_firmware_eth_url = shared_data.eth_firmware_url
        client_firmware = client["firmware"]
        firmware_info_available = current_firmware and current_firmware_wifi_url and current_firmware_eth_url
        client_update_required = client_firmware != "unknown" and client_firmware != current_firmware
        connect_mode_available = client_connect_mode in ["wifi", "ethernet"]
        if (firmware_info_available and client_update_required and connect_mode_available and not client["update_now"]):
            logger.info(f"{client_ip}:{client_port}:{client_node} !!!  new firmware ({current_firmware})")
            client["update_now"] = True
            new_firmware_url = current_firmware_wifi_url if client_connect_mode == "wifi" else current_firmware_eth_url
            payload = '{"payload":"update","url":"%s","delay":%d}' % (new_firmware_url, random.randint(1, 100))
            await websocket.send(payload)
            logger.info(f"{client_ip}:{client_port}:{client_node} <<< send firmware update: ({payload})")
        try:
            logger.debug(f"{client_ip}:{client_port}:{client_node}: check")
            await asyncio.sleep(1)
        except websockets.exceptions.ConnectionClosedError:
            logger.warning(f"{client_ip}:{client_port}:{client_node} !!! data stopped")
            break
        except Exception as e:
            logger.warning(f"{client_ip}:{client_port}:{client_node}: {e}")


async def echo(websocket, path):
    client_ip, client_port = websocket.remote_address
    logger.info(f"{client_ip}:{client_port} >>> new client")


    client = shared_data.clients[f"{client_ip}_{client_port}"] = {
        "firmware": "unknown",
        "connect_mode": "unknown",
        "update_now": False,
        "chip_id": "unknown",
        "node": "unknown",
        "grid": "unknown"
    }

    match path:
        case "/grid_detector":
            data_task = asyncio.create_task(loop_data(websocket, shared_data))
        case _:
            return

    try:
        while True:
            async for message in websocket:
                client_node = client["node"]
                client_firmware = client["firmware"]
                logger.info(f"{client_ip}:{client_port}:{client_node} >>> {message}")

                def split_message(message):
                    parts = message.split(":", 1)
                    header = parts[0]
                    data = parts[1] if len(parts) > 1 else ""
                    return header, data

                header, data = split_message(message)
                match header:
                    case "firmware":
                        client["firmware"] = data
                        client_firmware = data
                        logger.info(f"{client_ip}:{client_port}:{client_node} >>> firmware saved")
                    case "chip_id":
                        client["chip_id"] = data
                        logger.info(f"{client_ip}:{client_port}:{client_node} >>> chip_id saved")
                    case "pong":
                        logger.info(f"{client_ip}:{client_port}:{client_node} >>> pong")
                    case "node":
                        client["node"] = data
                        client_node = data
                        logger.info(f"{client_ip}:{client_port}:{client_node} >>> node {data}")
                        logger.info(f"{client_ip}:{client_port}:{client_node} >>> node saved")
                    case "grid":
                        client["grid"] = data
                        logger.info(f"{client_ip}:{client_port}:{client_node} >>> {data}")
                        await send_grid_status(data, client_node, False)
                    case "connect_mode":
                        client["connect_mode"] = data
                        logger.info(f"{client_ip}:{client_port}:{client_node} >>> connect mode {data}")
                    case "update":
                        current_status = data
                        logger.info(f"{client_ip}:{client_port}:{client_node} >>> firmware update status: {current_status}")
                    case _:
                        logger.info(f"{client_ip}:{client_port}:{client_node} !!! unknown data request")
    except websockets.exceptions.ConnectionClosedError as e:
        logger.error(f"Connection closed with error - {e}")
    except Exception as e:
        pass
    finally:
        data_task.cancel()
        del shared_data.clients[f"{client_ip}_{client_port}"]
        try:
            await data_task
        except asyncio.CancelledError:
            logger.info(f"{client_ip}:{client_port}:{client_firmware} !!! tasks cancelled")
        logger.info(f"{client_ip}:{client_port}:{client_firmware} !!! end")


async def update_shared_data(shared_data, mc):
    while True:
        try:
            logger.debug("update_shared_data")
            websoket_key = b"grid_detector_websocket_clients"
            await mc.set(websoket_key, json.dumps(shared_data.clients).encode("utf-8"))
            await asyncio.sleep(memcache_fetch_interval)
        except Exception as e:
            logger.error(f"Error in print_clients: {e}")

async def check_firmware(mc):
    while True:
        try:
            logger.debug("check_firmware")
            websoket_key = b"firmware_info"
            firmware_info = await mc.get(websoket_key)
            if firmware_info:
                shared_data.wifi_firmware_url = json.loads(firmware_info.decode("utf-8"))["firmware_wifi_url"]
                shared_data.eth_firmware_url = json.loads(firmware_info.decode("utf-8"))["firmware_eth_url"]
                shared_data.firmware_version = json.loads(firmware_info.decode("utf-8"))["firmware_version"]
            await asyncio.sleep(memcache_fetch_interval + 10)
        except Exception as e:
            logger.error(f"Error in check_firmware: {e}")


async def print_clients(shared_data, mc):
    while True:
        try:
            await asyncio.sleep(60)
            logger.info(f"-----\nClients:")
            for client, data in shared_data.clients.items():
                logger.info(f'{client}:{data["node"]}')
            logger.info(f"-----\n")
        except Exception as e:
            logger.error(f"Error in print_clients: {e}")


async def send_grid_status(status, node, test):
    data = {
        "blackout": status,
        "location": node,
        "test": test
    }
    async with aiohttp.ClientSession() as session:
        async with session.post(endpoint_url, json=data) as response:
            if response.status == 200:
                logger.info(f"location {node} blackout status {status}")
            else:
                logger.error(f"location {node} blackout status failed: {response.status}", )


async def update_grid_status(shared_data, mc):
    await asyncio.sleep(10)
    logger.debug("grid_status_start")

    nodes_status_memcached = await mc.get(b"grid_detector_nodes")
    if nodes_status_memcached:
        nodes_status = json.loads(nodes_status_memcached.decode("utf-8"))
    else:
        nodes_status = {}

    while True:
        try:
            logger.debug("grid_status_update")

            for node in nodes_list:
                node_found = False
                if not node in nodes_status:
                    nodes_status[node] = empty_node
                current_state = nodes_status.get(node, empty_node)
                for client_id, client in shared_data.clients.items():
                    if client["node"] == node:
                        node_found = True
                        status = client["grid"]
                        continue
                if not node_found:
                    status = "unknown"

                if current_state['grid'] != status:
                    status_change_time = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S+00:00")
                    logger.info(f"Node {node} status change: {current_state['grid']} >>> {status}")
                else:
                    status_change_time = nodes_status[node]['status_change_time']

                if (current_state['grid'] in ['online','offline']) and (status in ['online', 'offline']) and current_state['grid'] != status:
                    grid_change_time = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S+00:00")
                    logger.info(f"Node {node} grid change: {current_state['grid']} >>> {status}")
                    match environment:
                        case 'TEST':
                            test = True
                        case 'PROD':
                            test = False
                    match status:
                        case 'online':
                            status = False
                        case 'offline':
                            status = True
                    await send_grid_status(status, node, test)
                else:
                    grid_change_time = nodes_status[node]['grid_change_time']


                nodes_status[node] = {
                    "grid": status,
                    "grid_change_time": grid_change_time,
                    "status_change_time": status_change_time
                }

            await mc.set(b'grid_detector_nodes', json.dumps(nodes_status).encode("utf-8"))
            await asyncio.sleep(memcache_fetch_interval)
        except Exception as e:
            logger.error(f"Error in grid_status_update: {e}")
            await asyncio.sleep(memcache_fetch_interval)


start_server = websockets.serve(echo, "0.0.0.0", websocket_port, ping_interval=ping_interval)

asyncio.get_event_loop().run_until_complete(start_server)

update_shared_data_coroutine = partial(update_shared_data, shared_data, mc)()
asyncio.get_event_loop().create_task(update_shared_data_coroutine)
check_firmware_coroutine = partial(check_firmware, mc)()
asyncio.get_event_loop().create_task(check_firmware_coroutine)
print_clients_coroutine = partial(print_clients, shared_data, mc)()
asyncio.get_event_loop().create_task(print_clients_coroutine)
update_grid_status_coroutine = partial(update_grid_status, shared_data, mc)()
asyncio.get_event_loop().create_task(update_grid_status_coroutine)

asyncio.get_event_loop().run_forever()
