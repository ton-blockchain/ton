# rldp-http-proxy

> To interact with TON Sites, HTTP wrapped in RLDP is used. The hoster runs his site on any HTTP webserver and starts rldp-http-proxy next to it. All requests from the TON network come via the RLDP protocol to the proxy, and the proxy reassembles the request into simple HTTP and calls the original web server locally.

https://docs.ton.org/develop/network/rldp#rldp-http

## Uses

* [rldp](rldp)
* [rldp2](rldp2)
