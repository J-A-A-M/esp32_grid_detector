import asyncio
import websockets
import logging
import os
import json
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


class SharedData:
    def __init__(self):
        self.bins = "[]"
        self.clients = {}


shared_data = SharedData()


async def loop_data(websocket, client, shared_data):
    client_ip, client_port = websocket.remote_address
    client_firmware = client["firmware"]
    while True:
        try:
            logger.debug(f"{client_ip}:{client_port}:{client_firmware}: check")
            await asyncio.sleep(1)
        except websockets.exceptions.ConnectionClosedError:
            logger.warning(f"{client_ip}:{client_port}:{client_firmware} !!! data stopped")
            break
        except Exception as e:
            logger.warning(f"{client_ip}:{client_port}:{client_firmware}: {e}")


async def echo(websocket, path):
    client_ip, client_port = websocket.remote_address
    logger.info(f"{client_ip}:{client_port} >>> new client")


    client = shared_data.clients[f"{client_ip}_{client_port}"] = {
        "firmware": "unknown",
        "chip_id": "unknown",
        "grid": "unknown"
    }

    match path:
        case "/grid_detector":
            data_task = asyncio.create_task(loop_data(websocket, client, shared_data))
        case _:
            return

    try:
        while True:
            async for message in websocket:
                client_firmware = client["firmware"]
                logger.info(f"{client_ip}:{client_port}:{client_firmware} >>> {message}")

                def split_message(message):
                    parts = message.split(":", 1)
                    header = parts[0]
                    data = parts[1] if len(parts) > 1 else ""
                    return header, data

                header, data = split_message(message)
                match header:
                    case "firmware":
                        client["firmware"] = data
                        logger.info(f"{client_ip}:{client_port}:{client_firmware} >>> firmware saved")
                    case "chip_id":
                        client["chip_id"] = data
                        logger.info(f"{client_ip}:{client_port}:{client_firmware} >>> chip_id saved")
                    case "pong":
                        logger.info(f"{client_ip}:{client_port}:{client_firmware} >>> pong")
                    case "grid":
                        client["grid"] = data
                        logger.info(f"{client_ip}:{client_port}:{client_firmware} >>> {data}")
                    case _:
                        logger.info(f"{client_ip}:{client_port}:{client_firmware} !!! unknown data request")
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


async def print_clients(shared_data, mc):
    while True:
        try:
            await asyncio.sleep(60)
            logger.info(f"Clients:")
            for client, data in shared_data.clients.items():
                logger.info(client)

        except Exception as e:
            logger.error(f"Error in print_clients: {e}")


start_server = websockets.serve(echo, "0.0.0.0", websocket_port, ping_interval=ping_interval)

asyncio.get_event_loop().run_until_complete(start_server)

update_shared_data_coroutine = partial(update_shared_data, shared_data, mc)()
asyncio.get_event_loop().create_task(update_shared_data_coroutine)
print_clients_coroutine = partial(print_clients, shared_data, mc)()
asyncio.get_event_loop().create_task(print_clients_coroutine)

asyncio.get_event_loop().run_forever()
