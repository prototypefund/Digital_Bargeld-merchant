#This file is part of TALER
#Copyright (C) 2014, 2015, 2016, 2017 GNUnet e.V. and INRIA
#
#TALER is free software; you can redistribute it and/or modify it under the
#terms of the GNU Lesser General Public License as published by the Free Software
#Foundation; either version 2.1, or (at your option) any later version.
#
#TALER is distributed in the hope that it will be useful, but WITHOUT ANY
#WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
#A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.
#
#You should have received a copy of the GNU Lesser General Public License along with
#TALER; see the file COPYING.LGPL.  If not, see <http://www.gnu.org/licenses/>

# @author Marcello Stanisci
# @brief Error generator for responses coming from the exchange

from flask import (request,
                   Flask,
                   make_response)
import requests
from urllib.parse import (urljoin,
                          urlencode,
                          urlparse,
                          urlunparse)
from pytaler import amount
import base64
import os
import logging
import json
from random import randint
from datetime import datetime

app = Flask(__name__)
app.secret_key = base64.b64encode(os.urandom(64)).decode('utf-8')
logger = logging.getLogger(__name__)
exchange_url = os.environ.get("TALER_EXCHANGE_URL")
assert(None != exchange_url)

def track_transaction(resp):
    return resp.text

def track_transfer(resp):
    return resp.text

def keys(resp):
    try:
        keys = resp.json()
        # Put here data perturbation logic
        return json.dumps(keys)
    except Exception:
        return resp.text

@app.route('/', defaults={'path': ''})
@app.route('/<path:path>', methods=["GET", "POST"])
def all(path):
    body = request.get_json()
    url = list(urlparse(request.url))
    xurl = urlparse(exchange_url)
    url[0] = xurl[0]
    url[1] = xurl[1]
    url = urlunparse(url)
    if "POST" == request.method:
        r = requests.post(urljoin(url, path), json=body)
    else:
        r = requests.get(urljoin(url, path), json=body)
    dispatcher = {
        "track_transaction": track_transaction,
        "track_transfer": track_transfer,
        "keys": keys
    }
    func = dispatcher.get(request.headers.get("X-Taler-Mitm"),
                          lambda x: x.text)
    response = make_response(func(r))
    for key, value in r.headers.items():
        if key not in ("Server", "Content-Length"):
            response.headers[key] = value
    return response, r.status_code
