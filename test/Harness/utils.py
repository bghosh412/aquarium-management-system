import json
import urllib.request
import urllib.parse

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONFIG_PATH = ROOT / 'test' / 'Harness' / 'harness_config.json'

with open(CONFIG_PATH, 'r') as f:
    _CFG = json.load(f)

BASE_URL = _CFG.get('base_url', 'http://ams.local')
HTTP_TIMEOUT = _CFG.get('http_timeout', 5)


def http_get(path, params=None, timeout=HTTP_TIMEOUT):
    url = BASE_URL.rstrip('/') + path
    if params:
        url += '?' + urllib.parse.urlencode(params)
    req = urllib.request.Request(url)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as res:
            body = res.read().decode('utf-8')
            return res.getcode(), body
    except urllib.error.HTTPError as e:
        # Return HTTP error code and body for assertions to inspect
        try:
            body = e.read().decode('utf-8')
        except Exception:
            body = ''
        return e.code, body



def http_post_form(path, form_dict, timeout=HTTP_TIMEOUT):
    url = BASE_URL.rstrip('/') + path
    data = urllib.parse.urlencode(form_dict).encode('utf-8')
    req = urllib.request.Request(url, data=data, method='POST')
    req.add_header('Content-Type', 'application/x-www-form-urlencoded')
    try:
        with urllib.request.urlopen(req, timeout=timeout) as res:
            body = res.read().decode('utf-8')
            return res.getcode(), body
    except urllib.error.HTTPError as e:
        try:
            body = e.read().decode('utf-8')
        except Exception:
            body = ''
        return e.code, body


def assert_ok(cond, msg):
    if not cond:
        raise AssertionError(msg)


def load_config():
    return _CFG
