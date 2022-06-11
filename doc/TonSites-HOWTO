The aim of this document is to provide a gentle introduction into TON Sites, which are (TON) Web sites accessed through the TON Network. TON Sites may be used as a convenient entry point for other TON Services. In particular, HTML pages downloaded from TON Sites may contain links to ton://... URIs representing payments that can be performed by the user by clicking to the link, provided a TON Wallet is installed on the user's device. 

From the technical perspective, TON Sites are very much like the usual Web sites, but they are accessed through the TON Network (which is an overlay network inside the Internet) instead of the Internet. More specifically, they have an ADNL address (instead of a more customary IPv4 or IPv6 address), and they accept HTTP queries via RLDP protocol (which is a higher-level RPC protocol built upon ADNL, the main protocol of TON Network) instead of the usual TCP/IP. All encryption is handled by ADNL, so there is no need to use HTTPS (i.e., TLS).

In order to access existing and create new TON Sites one needs special gateways between the "ordinary" internet and the TON Network. Essentially, TON Sites are accessed with the aid of a HTTP->RLDP proxy running locally on the client's machine, and they are created by means of a reverse RLDP->HTTP proxy running on a remote web server.

1. Compiling RLDP-HTTP Proxy
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The RLDP-HTTP Proxy is a special utility specially designed for accessing and creating TON Sites. Its current (alpha) version is a part of the general TON Blockchain source tree, available at GitHub repository ton-blockchain/ton. In order to compile the RLDP-HTTP Proxy, follow the instructions outlined in README and Validator-HOWTO. The Proxy binary will be located as

    rldp-http-proxy/rldp-http-proxy

in the build directory. Alternatively, you may want to build just the Proxy instead of building all TON Blockchain projects. This can be done by invoking

    cmake --build . --target rldp-http-proxy

in the build directory.

2. Running RLDP-HTTP Proxy to access TON Sites
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In order to access existing TON Sites, you need a running instance of RLDP-HTTP Proxy on your computer. It can be invoked as follows:

   rldp-http-proxy/rldp-http-proxy -p 8080 -c 3333 -C global-config.json

or

   rldp-http-proxy/rldp-http-proxy -p 8080 -a <your_public_ip>:3333 -C global-config.json

where <your_public_ip> is your public IPv4 address, provided you have one on your home computer. The TON Network global configuration file `global-config.json` can be downloaded at https://ton.org/global-config.json :

   wget https://ton.org/global-config.json

In the above example, 8080 is the TCP port that will be listened to at localhost for incoming HTTP queries, and 3333 is the UDP port that will be used for all outbound and inbound RLDP and ADNL activity, i.e., for connecting to the TON Sites via the TON Network.

If you have done everything correctly, the Proxy will not terminate, but it will continue running in the terminal. It can be used now for accessing TON Sites. When you don't need it anymore, you can terminate it by pressing Ctrl-C, or simply by closing the terminal window.

3. Accessing TON Sites
~~~~~~~~~~~~~~~~~~~~~~

Now suppose that you have a running instance of the RLDP-HTTP Proxy running on your computer and listening on localhost:8080 for inbound TCP connections, as explained above in Section 2.

A simple test that everything is working property may be performed using programs such as Curl or WGet. For example,

    curl -x 127.0.0.1:8080 http://test.ton

attempts to download the main page of (TON) Site `test.ton` using the proxy at `127.0.0.1:8080`. If the proxy is up and running, you'll see something like

<HTML>
<H2>TON Blockchain Test Network &mdash; files and resources</H2>
<H3>News</H3>
<UL>
...
</HTML>

because TON Site `test.ton` is currently set up to be a mirror of Web Site https://ton.org.

You can also access TON Sites by means of their ADNL addresses by using fake domain `<adnl-addr>.adnl`:

    curl -x 127.0.0.1:8080 http://untzo7eat2h77xzfugxrfgfy3zbl5txomvetzke6fwr45lehvdkxauy.adnl/

currently fetches the same TON Web page.

Alternatively, you can set up `localhost:8080` as a HTTP proxy in your browser. For example, if you use Firefox, visit [Setup] -> General -> Network Settings -> Settings -> Configure Proxy Access -> Manual Proxy configuration, and type "127.0.0.1" into the field "HTTP Proxy", and "8080" into the field "Port". If you don't have Firefox yet, visit https://www.getfirefox.com first.

Once you have set up `localhost:8080` as the HTTP proxy to be used in your browser, you can simply type the required URI, such as `http://test.ton` or `http://untzo7eat2h77xzfugxrfgfy3zbl5txomvetzke6fwr45lehvdkxauy.adnl/`, in the navigation bar of your browser, and interact with the TON Site in the same way as with the usual Web Sites.

4. Creating TON Sites
~~~~~~~~~~~~~~~~~~~~~

Most people will need just to access existing TON Sites, not to create new ones. However, if you want to create one, you'll need to run RLDP-HTTP Proxy on your server, along with the usual web server software such as Apache or Nginx.

We suppose that you know already how to set up an ordinary web site, and that you have already configured one on your server, accepting incoming HTTP connections on TCP port <your-server-ip>:80, and defining the required TON Network domain name, say, `example.ton`, as the main domain name or an alias for your web site in the configuration of your web server.

After that, you first need to generate a persistent ADNL address for your server:

    mkdir keyring

    util/generate-random-id -m adnlid

You see something like

    45061C1D4EC44A937D0318589E13C73D151D1CEF5D3C0E53AFBCF56A6C2FE2BD vcqmha5j3ceve35ammfrhqty46rkhi455otydstv66pk2tmf7rl25f3

This is your newly-generated persistent ADNL address, in hexadecimal and user-friendly form. The corresponding private key is saved into file 45061...2DB in the current directory. Move it into the keyring directory:

    mv 45061C1* keyring/

After that, you execute

    rldp-http-proxy -a <your-server-ip>:3333 -L '*' -C global-config.json -A <your-adnl-address>

(with <your-adnl-address> equal to 'vcqm...35f3' in this example) in the background (you can try this in a terminal at first, but if you want your TON Site to run permanently, you'll have to use options `-d` and `-l <log-file>` as well).

If all works properly, the RLDP-HTTP proxy will accept incoming HTTP queries from the TON Network via RLDP/ADNL running on UDP port 3333 (of course, you can use any other UDP port if you want to) of IPv4 address <your-server-ip> (in particular, if you are using a firewall, don't forget to allow `rldp-http-proxy` to receive and send UDP packets from this port), and it will forward these HTTP queries addressed to all hosts (if you want to forward only specific hosts, change `-L '*'` to `-L <your hostname>`) to TCP port 80 at 127.0.0.1, i.e., to your ordinary Web server.

You can visit TON Site `http://<your-adnl-address>.adnl` (`http://vcqmha5j3ceve35ammfrhqty46rkhi455otydstv66pk2tmf7rl25f3.adnl` in this example) from a browser running on a client machine as explained in Sections 2 and 3 and check whether your TON Site is actually available to the public.

If you want to, you can register a TON DNS domain, such as 'example.ton', and create a record for this domain pointing to the persistent ADNL address of your TON Site. Then the RLDP-HTTP proxies running in client mode would resolve 'http://example.ton' as pointing to your ADNL address and will access your TON Site. The process of registration of TON DNS domains is described in a separate document.

