import os
import json
import uvicorn
import time
import logging

from starlette.applications import Starlette
from starlette.responses import JSONResponse, FileResponse, HTMLResponse
from starlette.routing import Route
from starlette.middleware import Middleware
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.exceptions import HTTPException
from starlette.requests import Request

from aiomcache import Client

from datetime import datetime, timezone

logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger(__name__)

debug = os.environ.get("DEBUG", False)

memcached_host = os.environ.get("MEMCACHED_HOST", "localhost")
memcached_port = int(os.environ.get("MEMCACHED_PORT", 11211))

data_token = os.environ.get("DATA_TOKEN") or None

mc = Client(memcached_host, memcached_port)

web_clients = {}


HTML_404_PAGE = """page not found"""
HTML_500_PAGE = """request error"""


async def not_found(request: Request, exc: HTTPException):
    logger.debug(f"Request time: {exc.args}")
    return HTMLResponse(content=HTML_404_PAGE)


async def server_error(request: Request, exc: HTTPException):
    logger.debug(f"Request time: {exc.args}")
    return HTMLResponse(content=HTML_500_PAGE)


exception_handlers = {404: not_found, 500: server_error}


class LogUserIPMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request, call_next):
        start_time = time.time()
        client_ip = request.client.host
        client_path = request.url.path

        match client_path:
            case "/":
                web_clients[client_ip] = [start_time, client_path]

        response = await call_next(request)
        elapsed_time = time.time() - start_time
        logger.debug(f"Request time: {elapsed_time}")
        return response


async def main(request):
    response = """
    <!DOCTYPE html>
    <html lang='en'>
    </html>
    """
    return HTMLResponse(response)


async def data(request):
    if request.path_params["token"] == data_token:
        etryvoga_full = await mc.get(b"grid_detector_websocket_clients")
        return JSONResponse(json.loads(etryvoga_full.decode("utf-8")))
    else:
        return JSONResponse({})


def get_local_time_formatted():
    local_time = datetime.now(timezone.utc)
    formatted_local_time = local_time.strftime("%Y-%m-%dT%H:%M:%SZ")
    return formatted_local_time


def calculate_time_difference(timestamp1, timestamp2):
    format_str = "%Y-%m-%dT%H:%M:%SZ"

    time1 = datetime.strptime(timestamp1, format_str)
    time2 = datetime.strptime(timestamp2, format_str)

    time_difference = (time2 - time1).total_seconds()
    return int(abs(time_difference))


middleware = [Middleware(LogUserIPMiddleware)]
app = Starlette(
    debug=debug,
    middleware=middleware,
    exception_handlers=exception_handlers,
    routes=[
        Route("/", main),
        Route("/data_{token}.json", data),
    ],
)

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8080)
