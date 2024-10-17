#!/usr/bin/env python3

import base64
import hashlib
import json
import subprocess

configname = "server.list"

binary = "../ton-build/generate-random-id"

ids = []
catchains = []
public_overlays = []

def ip2str(ip):
    b = ip.split(".")
    v = (int(b[0]) << 24) + (int(b[1]) << 16) + (int(b[2]) << 8) + int(b[3])
    return '-' + str((1 << 32) - v) if v >= (1 << 31) else str(v)

def get_addr_list(node):
    return json.dumps({
        "@type": "adnl.addressList",
        "version": 0,
        "addrs": [{
            "@type": "adnl.address.udp",
            "ip": ip2str(node['ip']),
            "port": node['port']
        }]
    })

def generate_dht_node(node):
    global binary
    addr_list = get_addr_list(node)
    result = subprocess.run([binary, '-k', node['spk'], '-a', addr_list, '-m', 'dht'], capture_output=True, text=True)
    return json.loads(result.stdout.split("\n")[0])

def add_id(s):
    global binary

    if not s or s[0] == '#':
        return

    t = s.split()
    assert len(t) >= 3
    ip, port, pk, *options = t
    pub = '-'
    short = '-'
    rand = None 
    spk = pk

    if pk[0] != '-':
        result = subprocess.run([binary, '-k', pk, '-m', 'id'], capture_output=True, text=True)
        output = result.stdout.split("\n")
        pk = json.loads(pk)
        pub = json.loads(output[1])
        short = json.loads(output[2])
        if '+dhtstatic' in options:
            if '+dht' not in options:
                options.append('+dht')
    else:
        if '+dhtstatic' in options:
            print("cannot use dht static on random node")
            import sys
            sys.exit(2)
        if any(opt.startswith('+catchain') for opt in options):
            print("cannot use catchain on random node")
            import sys
            sys.exit(2)
        rand = int(pk[1:])

    global ids
    if rand is None:
        ids.append({'ip': ip, 'port': port, 'options': options, 'pk': pk, 'spk': spk, 'pub': pub, 'short': short, 'rand': rand})
    else:
        for _ in range(rand):
            result = subprocess.run([binary, '-m', 'id'], capture_output=True, text=True)
            output = result.stdout.split("\n")
            pk = json.loads(output[0])
            pub = json.loads(output[1])
            short = json.loads(output[2])
            ids.append({'ip': ip, 'port': port, 'options': options, 'pk': pk, 'spk': spk, 'pub': pub, 'short': short, 'rand': None})

def readconfig(name):
    with open(name) as f:
        for line in f:
            add_id(line.strip())

def generate_global_config():
    global ids
  
    config = {'@type': 'config.global'}

    dht = {'@type': 'dht.config.global', 'k': 6, 'a': 3}

    dht_nodes = [generate_dht_node(node) for node in ids if '+dhtstatic' in node['options']]

    dht['static_nodes'] = {'@type': 'dht.nodes', 'nodes': dht_nodes} 

    config['dht'] = dht

    catchains = set(opt[9:] for node in ids for opt in node['options'] if opt.startswith('+catchain'))

    cc = []
    for name in catchains: 
        catchain = {
            '@type': 'catchain.config.global',
            'tag': base64.b64encode(hashlib.sha256(name.encode("utf-8")).digest()).decode("utf-8")
        }
        catchain_nodes = [node['pub'] for node in ids if f'+catchain{name}' in node['options']]
        catchain['nodes'] = catchain_nodes 
        cc.append(catchain)
    config['catchains'] = cc
    
    liteservers = [
        {
            '@type': 'liteservers.config.global',
            'ip': int(ip2str(node['ip'])),
            'port': 4924,
            'id': node['pub']
        }
        for node in ids
        for opt in node['options'] if opt.startswith('+liteserver')
    ]
    config['liteservers'] = liteservers

    validators = {
        "@type": "validator.config.global",
        "zero_state": {
            "workchain": -1,
            "shard": -9223372036854775808,
            "seqno": 0,
            "root_hash": "DXduYJkakj2d+rB7Qpj2SCkjTCG7AGXAA9G7EHQEyG0=",
            "file_hash": "4xASfDALy6Hk5Tg01IyBoFUepur9YJg2pg3cm0zocJk="
        }
    }

    config['validator'] = validators

    with open('ton-global.config.json', 'w') as outfile:
        json.dump(config, outfile, indent=2)

def generate_local_config(ip, port):
    global ids
    config = {'@type': 'config.local'}

    ports = sorted(set(int(node['port']) for node in ids if node['ip'] == ip and node['port'] == port))
    config['udp_ports'] = ports

    ids_config = [
        {'@type': 'id.config.local', 'id': node['pk']}
        for node in ids
        if node['rand'] is None and node['ip'] == ip and node['port'] == port
    ]
    config['local_ids'] = ids_config

    dht_config = []
    for node in ids:
        if node['ip'] == ip and node['port'] == port and ('+dht' in node['options']):
            if node['rand'] is None:
                dht_config.append({'@type': 'dht.config.local', 'id': node['short']})
            else:
                cnt = node['rand']
                dht_config.append({'@type': 'dht.config.random.local', 'cnt': cnt})
    config['dht'] = dht_config
    
    liteservers = [
        {'@type': 'liteserver.config.local', 'port': 4924, 'id': node['pk']}
        for node in ids
        for opt in node['options']
        if opt.startswith('+liteserver') and node['ip'] == ip and node['port'] == port
    ]
    config['liteservers'] = liteservers

    validators = [
        {'@type': 'validator.config.local', 'id': node['short']}
        for node in ids
        for opt in node['options']
        if opt == '+validator' and node['ip'] == ip and node['port'] == port
    ]
    config['validators'] = validators

    controlserver = {
        '@type': 'control.config.local',
        'priv': {"@type": "pk.ed25519", "key": "jRbqvPhSr3/xylof9zQyeqbplvWPSIGiHSft3ovKVc4="},
        'pub': 'Fv8DAtv6nqnrHIPpmv4LGIw0D9cMoF40JXQdM2WVMQM=',
        'port': (int(port) + 1000)
    }
    config['control'] = [controlserver]

    with open(f'ton-local.{ip}.{port}.config.json', 'w') as outfile:
        json.dump(config, outfile, indent=2)

readconfig(configname)
generate_global_config()

ips = set(f"{node['ip']}:{node['port']}" for node in ids)
for ip in ips:
    ip_addr, port = ip.split(":")
    generate_local_config(ip_addr, port)