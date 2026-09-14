from flask import Flask, send_from_directory
import os

web_root = os.path.dirname(__file__)
test = Flask(__name__)


@test.get('/')
def get_index():
    return send_from_directory(web_root, "index.html")


@test.get('/<path:path>')
def get_other(path):
    return send_from_directory(web_root, path)


@test.get('/api/status')
def get_api_status():
    return "hi"


@test.post('/api/logging/start')
def post_api_logging_start():
    return "hi"


@test.post('/api/logging/stop')
def post_api_logging_stop():
    return "hi"


@test.get('/api/logs')
def get_api_logs():
    return "hi"


@test.post('/api/live/start')
def post_api_live_start():
    return "hi"


@test.post('/api/live/stop')
def post_api_live_stop():
    return "hi"


@test.get('/api/live/signals')
def get_api_live_signals():
    return "hi"


if __name__ == '__main__':
    test.run()
