import asyncio
import websockets
import logging
import os
import json
import random
from aiomcache import Client
from functools import partial
from datetime import datetime, timezone, timedelta

debug_level = os.environ.get("DEBUG_LEVEL") or "DEBUG"
websocket_port = os.environ.get("WEBSOCKET_PORT") or 39447
ping_interval = int(os.environ.get("PING_INTERVAL", 20))
memcache_fetch_interval = int(os.environ.get("MEMCACHE_FETCH_INTERVAL", 1))

logging.basicConfig(level=debug_level, format="%(asctime)s %(levelname)s : %(message)s")
logger = logging.getLogger(__name__)


memcached_host = os.environ.get("MEMCACHED_HOST") or "localhost"
mc = Client(memcached_host, 11211)

nodes_list = {
    "t4a-9",
    "tr1-2",
    "rk2a-4"
}

empty_node = {
    "grid": "unknown",
    "grid_change_time": 0,
    "status_change_time": 0
}


class SharedData:
    def __init__(self):
        self.clients = {}
        self.firmware_url = ""
        self.firmware_version = ""


shared_data = SharedData()


async def loop_data(websocket, shared_data):
    client_ip, client_port = websocket.remote_address

    while True:
        client = shared_data.clients[f"{client_ip}_{client_port}"]
        client_node = client["node"]
        current_firmware = shared_data.firmware_version
        current_firmware_url = shared_data.firmware_url
        if (current_firmware and current_firmware_url) and (client["firmware"] != current_firmware):
            logger.info(f"{client_ip}:{client_port}:{client_node} !!! send firmware update ({current_firmware})")
            payload = '{"payload":"update","url":"%s","delay":%d}' % current_firmware_url, random.randint(1, 100)
            await websocket.send(payload)
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
                shared_data.firmware_url = json.loads(firmware_info.decode("utf-8"))["firmware_url"]
                shared_data.firmware_version = json.loads(firmware_info.decode("utf-8"))["firmware_version"]
            await asyncio.sleep(memcache_fetch_interval)
        except Exception as e:
            logger.error(f"Error in print_clients: {e}")


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

                if (current_state['grid'] in ['online','offline']) and (status in ['online','offline']) and current_state['grid'] != status:
                    grid_change_time = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S+00:00")
                    logger.info(f"Node {node} grid change: {current_state['grid']} >>> {status}")
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
check_firmware_coroutine = partial(check_firmware, mc)()
asyncio.get_event_loop().create_task(update_shared_data_coroutine)
print_clients_coroutine = partial(print_clients, shared_data, mc)()
asyncio.get_event_loop().create_task(print_clients_coroutine)
update_grid_status_coroutine = partial(update_grid_status, shared_data, mc)()
asyncio.get_event_loop().create_task(update_grid_status_coroutine)

asyncio.get_event_loop().run_forever()
