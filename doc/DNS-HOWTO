The aim of this document is to provide a very brief introduction to TON DNS, a service for translating human-readable domain names (such as `test.ton` or `mysite.temp.ton`) into TON smart contract addresses, ADNL addresses employed by services running in the TON Network (such as TON Sites), and so on.

1. Domain names
~~~~~~~~~~~~~~~

TON DNS employs familiarly-looking domain names, consisting of a UTF-8 encoded string up to 126 bytes, with different sections of the domain name separated by dots (`.`). Null characters (i.e. zero bytes) and, more generally, bytes in range 0..32 are not allowed in domain names. For instance, `test.ton` and `mysite.temp.ton` are valid TON DNS domains. A major difference from usual domain names is that TON DNS domains are case-sensitive; one could convert all domains to lowercase before performing a TON DNS lookup in order to obtain case-insensitivity if desired.

Currently, only domains ending in `.ton` are recognized as valid TON DNS domains. This could change in the future. Notice, however, that it is a bad idea to define first-level domains coinciding with first-level domains already existing in the Internet, such as `.com` or `.to`, because one could then register a TON domain `google.com`, deploy a TON site there, create a hidden link to a page at this TON site from his other innocently-looking TON site, and steal `google.com` cookies from unsuspecting visitors.

Internally, TON DNS transforms domain names as follows. First, a domain name is split into its components delimited by dot characters `.`. Then null characters are appended to each component, and all components are concatenated in reverse order. For example, `google.com` becomes `com\0google\0`.

2. Resolving TON DNS domains
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A TON DNS domain is resolved as follows. First, the *root DNS smart contract* is located by inspecting the value of configuration parameter #4 in a recent masterchain state. This parameter contains the 256-bit address of the root DNS smart contract inside the masterchain.

Then a special get-method `dnsresolve` (method id 123660) is invoked for the root DNS smart contract, with two parameters. The first parameter is a CellSlice with *8n* data bits containing the internal representation of the domain being resolved, where *n* is the length of the internal representation in bytes (at most 127). The second parameter is a signed 16-bit Integer containing the required *category*. If the category is zero, then all categories are requested.

If this get-method fails, then the TON DNS lookup is unsuccessful. Otherwise the get-method returns two values. The first is *8m*, the length (in bits) of the prefix of the internal representation of the domain that has been resolved, 0 < m <= n. The second is a Cell with the TON DNS record for the required domain in the required category, or the root a Dictionary with 16-bit signed integer keys (categories) and values equal to the serializations of corresponding TON DNS records. If the domain cannot be resolved by the root DNS smart contract, i.e. if no non-empty prefix is a valid domain known to the smart contract, then (0, null) is returned. In other words, m = 0 means that the TON DNS lookup has found no data for the required domain. In that case, the TON DNS lookup is also unsuccessful.

If m = n, then the second component of the result is either a Cell with a valid TON DNS record for the required domain and category, or a Null if there is no TON DNS record for this domain with this category. In either case, the resolution process stops, and the TON DNS record thus obtained is deserialized and the required information (such as the type of the record and its parameters, such as a smart contract address or a ADNL address).

Finally, if m < n, then the lookup is successful so far, but only a partial result is available for the m-byte prefix of the original internal representation of the domain. The longest of all such prefixes known to the DNS smart contract is returned. For instance, an attempt to look up `mysite.test.ton` (i.e. `ton\0test\0mysite\0` in the internal representation) in the root DNS smart contract might return 8m=72, corresponding to prefix `ton\0test\0`, i.e. to subdomain "test.ton" in the usual domain representation. In that case, dnsresolve() returns the value for category -1 for this prefix regardless of the category originally requested by the client. By convention, category -1 usually contains a TON DNS Record of type *dns_next_resolver*, containing the address of next resolver smart contract (which can reside in any other workchain, such as the basechain). If that is indeed the case, the resolution process continues by running get-method `dnsresolve` for the next resolver, with the internal representation of the domain name containing only its part unresolved so far (if we were looking up `ton\0test\0mysite\0`, and prefix `ton\0test\0` was found by the root DNS smart contract, then the next `dnsresolve` will be invoked with `mysite\0` as its first argument). Then either the next resolver smart contract reports an error or the absence of any records for the required domain or any of its prefixes, or the final result is obtained, or another prefix and next resolver smart contract is returned. In the latter case, the process continues in the same fashion until all of the original domain is resolved.

3. Using LiteClient and TonLib to resolve TON DNS domains
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The above process can be invoked automatically using the TON LiteClient or TONLib. For instance, one can invoke the command `dnsresolve test.ton 1` in the LiteClient to resolve "test.ton" with category 1 and obtain the following result:

================================================
> dnsresolve test.ton
...
Result for domain 'test.ton' category 1
raw data: x{AD011B3CBBE404F47FFEF92D0D7894C5C6F215F677732A49E544F16D1E75643D46AB00}

category #1 : (dns_adnl_address adnl_addr:x1B3CBBE404F47FFEF92D0D7894C5C6F215F677732A49E544F16D1E75643D46AB flags:0)
	adnl address 1B3CBBE404F47FFEF92D0D7894C5C6F215F677732A49E544F16D1E75643D46AB = UNTZO7EAT2H77XZFUGXRFGFY3ZBL5TXOMVETZKE6FWR45LEHVDKXAUY
================================================

In this case, the TON DNS record for "test.ton" is a `dns_adnl_address` record containing ADNL address UNTZO7EAT2H77XZFUGXRFGFY3ZBL5TXOMVETZKE6FWR45LEHVDKXAUY

Alternatively, one can invoke `tonlib-cli` and enter the following command:
================================================
> dns resolve root test.ton 1
Redirect resolver
...
Done
  test.ton 1 ADNL:untzo7eat2h77xzfugxrfgfy3zbl5txomvetzke6fwr45lehvdkxauy
================================================

This is a more compact representation of the same result.

Finally, if one uses RLDP-HTTP Proxy in the client mode to access TON Sites from a browser as explained in `TONSites-HOWTO.txt`, the TONLib resolver is automatically invoked to resolve all domains entered by the end user, so that a HTTP query to `http://test.ton/testnet/last` is automatically forwarded to ADNL address `untzo7eat2h77xzfugxrfgfy3zbl5txomvetzke6fwr45lehvdkxauy` via RLDP.

4. Registering new domains
~~~~~~~~~~~~~~~~~~~~~~~~~~

Suppose that you have a new TON Site with a newly-generated ADNL address, such as `vcqmha5j3ceve35ammfrhqty46rkhi455otydstv66pk2tmf7rl25f3`. Of course, the end user might type `http://vcqmha5j3ceve35ammfrhqty46rkhi455otydstv66pk2tmf7rl25f3.adnl/` to visit your TON Site from a browser using a RLDP-HTTP Proxy in client mode, but this is not very convenient. Instead, you could register a new domain, say, `mysite.temp.ton` with a `dns_adnl_address` record in category 1 containing the ADNL address vcq...25f3 of your TON Site. Then the user would access your TON Site by simply typing `mysite.temp.ton` in a browser.

In general, you would need to contact the owner of the higher-level domain and ask him to add a record for your subdomain in his DNS resolver smart contract. However, the TestNet of the TON Blockchain has a special resolver smart contract for `temp.ton` that allows anyone to automatically register any subdomains of `temp.ton` not registered yet, provided a small fee (in test Grams) is paid to that smart contract. In our case, we first need to find out the address of this smart contract, for example by using the Lite Client:

================================================
> dnsresolve temp.ton -1
...
category #-1 : (dns_next_resolver
  resolver:(addr_std
    anycast:nothing workchain_id:0 address:x190BD756F6C0E7948DC26CB47968323177FB20344F8F9A50918CAF87ECB34B79))
	next resolver 0:190BD756F6C0E7948DC26CB47968323177FB20344F8F9A50918CAF87ECB34B79 = EQAZC9dW9sDnlI3CbLR5aDIxd_sgNE-PmlCRjK-H7LNLeUXN
================================================

We see that the address of this automatic DNS smart contract is EQAZC9dW9sDnlI3CbLR5aDIxd_sgNE-PmlCRjK-H7LNLeUXN. We can run several get methods to compute the required price for registering a subdomain, and to learn the period for which the subdomain will be registered:

================================================
> runmethod EQAZC9dW9sDnlI3CbLR5aDIxd_sgNE-PmlCRjK-H7LNLeUXN getstdperiod
...
arguments:  [ 67418 ] 
result:  [ 700000 ] 
remote result (not to be trusted):  [ 700000 ] 
> runmethod EQAZC9dW9sDnlI3CbLR5aDIxd_sgNE-PmlCRjK-H7LNLeUXN getppr
...
arguments:  [ 109522 ] 
result:  [ 100000000 ] 
remote result (not to be trusted):  [ 100000000 ] 
================================================

We see that subdomains are registered for 700000 seconds (about eight days), and that the registration price is 100000000ng = 0.1 test Grams per domain, plus a price for each bit and cell of stored data, which can be learned by running get-methods `getppb` and `getppc`.

Now we want this smart contract to register our subdomain. In order to do this, we have to create a special message from our wallet to the automatic DNS smart contract. Let us assume that we have a wallet `my_new_wallet` with address kQABzslAMKOVwkSkkWfelS1pYSDOSyTcgn0yY_loQvyo_ZgI. Then we run the following Fift script (from the subdirectory `crypto/smartcont` of the source tree):

    fift -s auto-dns.fif <auto-dns-smc-addr> add <my-subdomain> <expire-time> owner <my-wallet-addr> cat 1 adnl <my-site-adnl-address>

For example:

===============================================
$ fift -s auto-dns.fif EQAZC9dW9sDnlI3CbLR5aDIxd_sgNE-PmlCRjK-H7LNLeUXN add 'mysite' 700000 owner kQABzslAMKOVwkSkkWfelS1pYSDOSyTcgn0yY_loQvyo_ZgI cat 1 adnl vcqmha5j3ceve35ammfrhqty46rkhi455otydstv66pk2tmf7rl25f3
Automatic DNS smart contract address = 0:190bd756f6c0e7948dc26cb47968323177fb20344f8f9a50918caf87ecb34b79 
kQAZC9dW9sDnlI3CbLR5aDIxd_sgNE-PmlCRjK-H7LNLef5H
Action: add mysite 1583865040 
Operation code: 0x72656764 
Value: x{2_}
 x{BC000C_}
  x{AD0145061C1D4EC44A937D0318589E13C73D151D1CEF5D3C0E53AFBCF56A6C2FE2BD00}
 x{BFFFF4_}
  x{9FD3800039D928061472B84894922CFBD2A5AD2C2419C9649B904FA64C7F2D085F951FA01_}

Internal message body is: x{726567645E5D2E700481CE3F0EDAF2E6D2E8CA00BCCFB9A1_}
 x{2_}
  x{BC000C_}
   x{AD0145061C1D4EC44A937D0318589E13C73D151D1CEF5D3C0E53AFBCF56A6C2FE2BD00}
  x{BFFFF4_}
   x{9FD3800039D928061472B84894922CFBD2A5AD2C2419C9649B904FA64C7F2D085F951FA01_}

B5EE9C7241010601007800012F726567645E5D2E700481CE3F0EDAF2E6D2E8CA00BCCFB9A10102012002030105BC000C040105BFFFF4050046AD0145061C1D4EC44A937D0318589E13C73D151D1CEF5D3C0E53AFBCF56A6C2FE2BD0000499FD3800039D928061472B84894922CFBD2A5AD2C2419C9649B904FA64C7F2D085F951FA01070E6337D
Query_id is 6799642071046147647 = 0x5E5D2E700481CE3F 
(Saved to file dns-msg-body.boc)
================================================

We see that the internal message body for this query has been created and saved into file `dns-msg-body.boc`. Now you have to send a payment from your wallet kQAB..ZgI to the automatic DNS smart contract EQA..UXN, along with message body from file `dns-msg-body.boc`, so that the automatic DNS smart contract knows what you want it to do. If your wallet has been created by means of `new-wallet.fif`, you can simply use `-B` command-line argument to `wallet.fif` while performing this transfer:

================================================
$ fift -s wallet.fif my_new_wallet EQAZC9dW9sDnlI3CbLR5aDIxd_sgNE-PmlCRjK-H7LNLeUXN 1 1.7 -B dns-msg-body.boc
Source wallet address = 0:01cec94030a395c244a49167de952d696120ce4b24dc827d3263f96842fca8fd 
kQABzslAMKOVwkSkkWfelS1pYSDOSyTcgn0yY_loQvyo_ZgI
Loading private key from file my_new_wallet.pk
Transferring GR$1.7 to account kQAZC9dW9sDnlI3CbLR5aDIxd_sgNE-PmlCRjK-H7LNLef5H = 0:190bd756f6c0e7948dc26cb47968323177fb20344f8f9a50918caf87ecb34b79 seqno=0x1 bounce=-1 
Body of transfer message is x{726567645E5D2E700481CE3F0EDAF2E6D2E8CA00BCCFB9A1_}
 x{2_}
  x{BC000C_}
   x{AD0145061C1D4EC44A937D0318589E13C73D151D1CEF5D3C0E53AFBCF56A6C2FE2BD00}
  x{BFFFF4_}
   x{9FD3800039D928061472B84894922CFBD2A5AD2C2419C9649B904FA64C7F2D085F951FA01_}

signing message: x{0000000103}
 x{62000C85EBAB7B6073CA46E1365A3CB41918BBFD901A27C7CD2848C657C3F659A5BCA32A9F880000000000000000000000000000726567645E5D2E700481CE3F0EDAF2E6D2E8CA00BCCFB9A1_}
  x{2_}
   x{BC000C_}
    x{AD0145061C1D4EC44A937D0318589E13C73D151D1CEF5D3C0E53AFBCF56A6C2FE2BD00}
   x{BFFFF4_}
    x{9FD3800039D928061472B84894922CFBD2A5AD2C2419C9649B904FA64C7F2D085F951FA01_}

resulting external message: x{8800039D928061472B84894922CFBD2A5AD2C2419C9649B904FA64C7F2D085F951FA050E3817FC01F564AECE810B8077D72E3EE15C81392E8B4AE9CDD0D6575821481C996AE8FFBABA0513F131E10E27C006C6544E99D71E0A6AACF7D02C677342B040000000081C_}
 x{62000C85EBAB7B6073CA46E1365A3CB41918BBFD901A27C7CD2848C657C3F659A5BCA32A9F880000000000000000000000000000726567645E5D2E700481CE3F0EDAF2E6D2E8CA00BCCFB9A1_}
  x{2_}
   x{BC000C_}
    x{AD0145061C1D4EC44A937D0318589E13C73D151D1CEF5D3C0E53AFBCF56A6C2FE2BD00}
   x{BFFFF4_}
    x{9FD3800039D928061472B84894922CFBD2A5AD2C2419C9649B904FA64C7F2D085F951FA01_}

B5EE9C72410207010001170001CF8800039D928061472B84894922CFBD2A5AD2C2419C9649B904FA64C7F2D085F951FA050E3817FC01F564AECE810B8077D72E3EE15C81392E8B4AE9CDD0D6575821481C996AE8FFBABA0513F131E10E27C006C6544E99D71E0A6AACF7D02C677342B040000000081C01019762000C85EBAB7B6073CA46E1365A3CB41918BBFD901A27C7CD2848C657C3F659A5BCA32A9F880000000000000000000000000000726567645E5D2E700481CE3F0EDAF2E6D2E8CA00BCCFB9A10202012003040105BC000C050105BFFFF4060046AD0145061C1D4EC44A937D0318589E13C73D151D1CEF5D3C0E53AFBCF56A6C2FE2BD0000499FD3800039D928061472B84894922CFBD2A5AD2C2419C9649B904FA64C7F2D085F951FA01031E3A74C
(Saved to file wallet-query.boc)
=====================================================

(You have to replace 1 with the correct sequence number for your wallet.) Once you obtain a signed external message in `wallet-query.boc`, addressed to your wallet and instructing it to transfer 1.7 test Grams to the automatic DNS smart contract along with the description of your new domain to be registered, you can upload this message using the LiteClient by typing

=====================================================
> sendfile wallet-query.boc 
[ 1][t 1][!testnode]	sending query from file wallet-query.boc
[ 3][t 1][!query]	external message status is 1
=====================================================

If all works correctly, you'll obtain some change from the automatic DNS smart contract in a confirmation message (it will charge only the storage fees for your subdomain and processing fees for running the smart contract and sending messages, and return the rest), and your new domain will be registered:

=====================================================
> last
...
> dnsresolve mysite.temp.ton 1
...
Result for domain 'mysite.temp.ton' category 1
category #1 : (dns_adnl_address adnl_addr:x45061C1D4EC44A937D0318589E13C73D151D1CEF5D3C0E53AFBCF56A6C2FE2BD flags:0)
	adnl address 45061C1D4EC44A937D0318589E13C73D151D1CEF5D3C0E53AFBCF56A6C2FE2BD = vcqmha5j3ceve35ammfrhqty46rkhi455otydstv66pk2tmf7rl25f3
=====================================================

You can modify or prolong this domain in essentially the same manner, by first creating a request in file `dns-msg-body.boc` by means of `auto-dns.fif`, using such actions as `update` or `prolong`, and then embedding this request into a message from your wallet to the automatic DNS smart contract using `wallet.fif` or a similar script with command-line argument `-B dns-msg-body.boc`.
