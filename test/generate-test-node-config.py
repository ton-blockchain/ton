#!/bin/python3.7

import subprocess
import json
import pprint
import base64
import hashlib

configname="server.list"

binary="../ton-build/generate-random-id"

ids=[]
catchains=[]
public_overlays=[]

def ip2str(ip):
    b = ip.split(".")
    v = (int(b[0]) << 24) + (int(b[1]) << 16) + (int(b[2]) << 8) + int(b[3])
    if (v >= (1 << 31)):
        return '-' + str((1 << 32) - v)
    else:
        return str(v)

def get_addr_list(node):
    return '{"@type":"adnl.addressList","version":0,"addrs":[{"@type":"adnl.address.udp","ip":'+ \
        ip2str(node['ip'])+',"port":'+node['port']+'}]}'

def generate_dht_node(node):
    global binary
    addr_list = get_addr_list(node)
    r = subprocess.run([binary, '-k', node['spk'], '-a', addr_list, '-m', 'dht'], capture_output=True) 
    s = r.stdout.decode("utf-8").split("\n")
    return json.loads(s[0])

def add_id(s):
    global binary

    if len(s) < 1 or s[0] == '#':
        return

    t = s.split(" ")
    assert(len(t) >= 3)
    ip = t[0]
    port = t[1]
    pk = t[2]
    options = t[3:]
    pub = '-'
    short = '-'
    rand = None 
    spk = pk

    if (pk[0] != '-'):
        r = subprocess.run([binary, '-k', pk, '-m', 'id'], capture_output=True) 
        s = r.stdout.decode("utf-8").split("\n")
        pk = json.loads(pk)
        pub = json.loads(s[1])
        short = json.loads(s[2])
        if '+dhtstatic' in options:
            if not '+dht' in options:
                options.append('+dht')
    else:
        if '+dhtstatic' in options:
            print("cannot use dht static on random node")
            sys.exit(2)
        if any(opt.startswith('+catchain') for opt in options):
            print("cannot use catchain on random node")
            sys.exit(2)
        rand=int(pk[1:])

    global ids
    if rand == None:
        ids.append({'ip' : ip, 'port' : port, 'options' : options, 'pk' : pk, 'spk' : spk, 'pub' : pub, 'short' : short, 'rand' : rand})
    else:
        for x in range(0, rand):
            r = subprocess.run([binary, '-m', 'id'], capture_output=True) 
            s = r.stdout.decode("utf-8").split("\n")
            pk = json.loads(s[0])
            pub = json.loads(s[1])
            short = json.loads(s[2])
            ids.append({'ip' : ip, 'port' : port, 'options' : options, 'pk' : pk, 'spk' : spk, 'pub' : pub, 'short' : short, 'rand' : None})

def readconfig(name):
    with open(name) as f: 
        for s in f.readlines():
            add_id(s.strip())

def generate_global_config():
    global ids
  
    config={'@type':'config.global'}

    dht={'@type':'dht.config.global','k':6,'a':3}

    dht_nodes=[]
    for node in ids:
        if '+dhtstatic' in node['options']:
            dht_nodes.append(generate_dht_node(node))

    dht['static_nodes'] = {'@type':'dht.nodes','nodes':dht_nodes} 

    config['dht'] = dht

    catchains = []
    for x in ids:
        for y in x['options']:
            if y.startswith('+catchain'):
                assert(x['rand'] == None)
                name=y[9:]
                if not name in catchains:
                    catchains.append(name)

    cc=[]
    for name in catchains: 
        catchain={'@type':'catchain.config.global','tag':base64.b64encode(hashlib.sha256(name.encode("utf-8")).digest()).decode("utf-8")}
        catchain_nodes=[]
        for node in ids:
            if ('+catchain'+name) in node['options']:
                catchain_nodes.append(node['pub'])
        catchain['nodes'] = catchain_nodes 
        cc.append(catchain)
    config['catchains'] = cc
    
    liteservers=[]
    for node in ids:
        for opt in node['options']:
            if opt.startswith('+liteserver'):
                assert(node['rand'] == None)
                name=opt[9:]
                liteservers.append({'@type':'liteservers.config.global','ip':int(ip2str(node['ip'])), 'port':4924,'id':node['pub']})
    config['liteservers'] = liteservers

    validators = {"@type": "validator.config.global", "zero_state": {
        "workchain" : -1,
        "shard" : -9223372036854775808,
        "seqno" : 0,
        "root_hash": "DXduYJkakj2d+rB7Qpj2SCkjTCG7AGXAA9G7EHQEyG0=", 
        "file_hash": "4xASfDALy6Hk5Tg01IyBoFUepur9YJg2pg3cm0zocJk="
        }}

    config['validator'] = validators

    with open('ton-global.config.json', 'w') as outfile:
        json.dump(config, outfile, indent=2)

def generate_local_config(ip,port):
    global ids
    config = {'@type' : 'config.local'}

    ports=[]
    for node in ids:
        if node['ip'] == ip and node['port'] == port:
            ports.append(int(node['port']))
    ports = sorted(set(ports))
    config['udp_ports'] = ports

    ids_config=[]
    for node in ids:
        if node['rand'] == None and node['ip'] == ip and node['port'] == port:
            ids_config.append({'@type':'id.config.local','id':node['pk']})
    config['local_ids'] = ids_config


    dht_config=[]
    for node in ids:
        if node['ip'] == ip and node['port'] == port and ('+dht' in node['options']):
            if node['rand'] == None:
                dht_config.append({'@type':'dht.config.local', 'id':node['short']})
            else:
                cnt=node['rand']
                dht_config.append({'@type':'dht.config.random.local', 'cnt':cnt})
    config['dht'] = dht_config
    
    liteservers=[]
    for node in ids:
        for opt in node['options']:
            if opt.startswith('+liteserver') and node['ip'] == ip and node['port'] == port:
                assert(node['rand'] == None)
                name=opt[9:]
                liteservers.append({'@type':'liteserver.config.local','port':4924,'id':node['pk']})
    config['liteservers'] = liteservers

    validators=[]
    for node in ids:
        for opt in node['options']:
            if opt == '+validator' and node['ip'] == ip and node['port'] == port:
                assert(node['rand'] == None)
                validators.append({'@type':'validator.config.local', 'id':node['short']})
    config['validators'] = validators

    controlserver = {'@type':'control.config.local', 'priv':{"@type":"pk.ed25519","key":"jRbqvPhSr3/xylof9zQyeqbplvWPSIGiHSft3ovKVc4="}, \
            'pub':'Fv8DAtv6nqnrHIPpmv4LGIw0D9cMoF40JXQdM2WVMQM=', 'port':(int(port) + 1000)}
    config['control'] = [controlserver]

    with open('ton-local.' + ip + '.' + port + '.config.json', 'w') as outfile:
        json.dump(config, outfile, indent=2)



readconfig(configname)
generate_global_config()

ips=["" + node['ip'] + ":" + node['port'] for node in ids]
ips=set(ips)
for ip in ips:
    b = ip.split(":")
    generate_local_config(b[0], b[1])
