import asyncio
import websockets
import logging
import os
import json
import random
from aiomcache import Client
from functools import partial
from datetime import datetime, timezone, timedelta
from ga4mp import GtagMP
from ga4mp.store import DictStore

debug_level = os.environ.get("DEBUG_LEVEL") or "DEBUG"
websocket_port = os.environ.get("WEBSOCKET_PORT") or 39447
ping_interval = int(os.environ.get("PING_INTERVAL", 20))
memcache_fetch_interval = int(os.environ.get("MEMCACHE_FETCH_INTERVAL", 60))
environment = os.environ.get("ENVIRONMENT") or "PROD"

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
    while True:
        match client["firmware"]:
            case "unknown":
                client_id = client_port
            case _:
                client_id = client["firmware"]
        try:
            logger.debug(f"{client_ip}:{client_id}: check")
            await asyncio.sleep(1)
        except websockets.exceptions.ConnectionClosedError:
            logger.warning(f"{client_ip}:{client_id} !!! data stopped")
            break
        except Exception as e:
            logger.warning(f"{client_ip}:{client_id}: {e}")


async def echo(websocket, path):
    client_ip, client_port = websocket.remote_address
    logger.info(f"{client_ip}:{client_port} >>> new client")


    client = shared_data.clients[f"{client_ip}_{client_port}"] = {
        "firmware": "unknown",
        "chip_id": "unknown",
    }

    match path:
        case "/grid_detector":
            data_task = asyncio.create_task(loop_data(websocket, client, shared_data))
        case _:
            return

    try:
        while True:
            async for message in websocket:
                match client["firmware"]:
                    case "unknown":
                        client_id = client_port
                    case _:
                        client_id = client["firmware"]
                logger.info(f"{client_ip}:{client_id} >>> {message}")

                def split_message(message):
                    parts = message.split(":", 1)  # Split at most into 2 parts
                    header = parts[0]
                    data = parts[1] if len(parts) > 1 else ""
                    return header, data

                header, data = split_message(message)
                match header:
                    case "firmware":
                        client["firmware"] = data
                        logger.warning(f"{client_ip}:{client_id} >>> firmware saved")
                    case "chip_id":
                        client["chip_id"] = data
                        logger.info(f"{client_ip}:{client_id} >>> chip_id saved")
                    case "pong":
                        logger.info(f"{client_ip}:{client_id} >>> pong")
                    case "grid":
                        logger.info(f"{client_ip}:{client_id} >>> {data}")
                    case _:
                        logger.info(f"{client_ip}:{client_id} !!! unknown data request")
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
            logger.info(f"{client_ip}:{client_port} !!! tasks cancelled")
        logger.info(f"{client_ip}:{client_port} !!! end")


async def update_shared_data(shared_data, mc):
    while True:
        logger.debug("memcache check")
        await asyncio.sleep(memcache_fetch_interval)


async def print_clients(shared_data, mc):
    while True:
        try:
            await asyncio.sleep(60)
            logger.info(f"Clients:")
            for client, data in shared_data.clients.items():
                logger.info(client)
            websoket_key = b"grid_detector_websocket_clients"
            await mc.set(websoket_key, json.dumps(shared_data.clients).encode("utf-8"))
        except Exception as e:
            logger.error(f"Error in update_shared_data: {e}")


start_server = websockets.serve(echo, "0.0.0.0", websocket_port, ping_interval=ping_interval)

asyncio.get_event_loop().run_until_complete(start_server)

update_shared_data_coroutine = partial(update_shared_data, shared_data, mc)()
asyncio.get_event_loop().create_task(update_shared_data_coroutine)
print_clients_coroutine = partial(print_clients, shared_data, mc)()
asyncio.get_event_loop().create_task(print_clients_coroutine)

asyncio.get_event_loop().run_forever()
