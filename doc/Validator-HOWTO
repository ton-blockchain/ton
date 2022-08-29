The aim of this document is to provide step-by-step instructions for setting up a full node for the TON Blockchain as a validator. We assume that a TON Blockchain Full Node is already up and running as explained in FullNode-HOWTO. We also assume some familiarity with the TON Blockchain Lite Client.

Note that a validator must be run on a dedicated high-performance server with high network bandwidth installed in a reliable datacenter, and that you'll need a large amount of Grams (test Grams, if you want to run a validator in the "testnet") as stakes for your validator. If your validator works incorrectly or is not available for prolonged periods of time, you may lose part or all of your stake, so it makes sense to use high-performance, reliable servers. We recommend a dual-processor server with at least eight cores in each processor, at least 256 MiB RAM, at least 8 TB of conventional HDD storage and at least 512 GB of faster SSD storage, with 1 Gbit/s network (and Internet) connectivity to reliably accomodate peak loads.

0. Downloading and compiling
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The basic instructions are the same as for a TON Blockchain Full Node, as explained in FullNode-HOWTO. In fact, any Full Node will automatically work as a validator if it discovers that the public key corresponding to its private key appears as a member of the current validator set for the currently selected TON Blockchain instance. In particular, the Full Node and the Validator use the same binary file `validator-engine`, and are controlled by means of the same `validator-engine-console`.

1. Controlling smart contract of a validator
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In order to run a Validator, you'll need a Full Node that is already up and running (and completely synchronized with the current blockchain state), and a wallet in the masterchain holding a large amount of Grams (or test Grams, if you want to run a validator in the "testnet" TON Blockchain instance). Typically you'll need at least 100,001 Grams in the production network, and at least 10,001 test Grams in the test network. The actual value (in nanograms) can be found as the value of `min_stake` in configuration parameter #17 (available by typing `getconfig 17` into the Lite Client), plus one Gram.

Each validator is identified by its (Ed25519) public key. During the validator elections, the validator (or rather its public key) is also associated with a smart contract residing in the masterchain. For simplicity, we say that the validator is "controlled" by this smart contract (e.g., a wallet smart contract). Stakes are accepted on behalf of this validator only if they arrive from its associated smart contract, and only that associated smart contract is entitled to collect the validator's stake after it is unfrozen, along with the validator's share of bonuses (e.g., block mining fees, transaction and message forwarding fees collected from the users of the TON Blockchain by the validator pool). Typically the bonuses are distributed proportionally to the (effective) stakes of the validators. On the other hand, validators with higher stakes are assigned a larger amount of work to perform (i.e., they have to create and validate blocks for more shardchains), so it is important not to stake an amount that will yield more validation work than your node is capable of handling.

Notice that each validator (identified by its public key) can be associated with at most one controlling smart contract (residing in the masterchain), but the same controlling smart contract may be associated with several validators. In this way you can run several validators (on different physical servers) and make stakes for them from the same smart contract. If one of these validators stops functioning and you lose its stake, the other validators should continue operating and will keep their stakes and potentially receive bonuses.

2. Creating the controlling smart contract
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If you don't have a controlling smart contract, you can simply create a wallet in the masterchain. A simple wallet can be created with the aid of the script new-wallet.fif, located in the subdirectory crypto/smartcont of the source tree. In what follows, we assume that you have configured the environment variable FIFTPATH to include <source-root>/crypto/fift/lib:<source-root>/crypto/smartcont, and that your PATH includes a directory with the Fift binary (located as <build-directory>/crypto/fift). Then you can simply run

$ fift -s new-wallet.fif -1 my_wallet_id

where "my_wallet_id" is any identifier you want to assign to your new wallet, and -1 is the workchain identifier for the masterchain. If you have not set up FIFTPATH and PATH, then you'll have to run a longer version of this command in your build directory as follows:

$ crypto/fift -I <source-dir>/crypto/fift/lib:<source-dir>/crypto/smartcont -s new-wallet.fif -1 my_wallet_id

Once you run this script, the address of the new smart contract is displayed:
...
new wallet address = -1:af17db43f40b6aa24e7203a9f8c8652310c88c125062d1129fe883eaa1bd6763 
(Saving address to file my_wallet_id.addr)
Non-bounceable address (for init): 0f-vF9tD9Atqok5yA6n4yGUjEMiMElBi0RKf6IPqob1nYzqK
Bounceable address (for later access): kf-vF9tD9Atqok5yA6n4yGUjEMiMElBi0RKf6IPqob1nY2dP
...
(Saved wallet creating query to file my_wallet_id-query.boc)

Now my_wallet_id.pk is a new file containing the private key for controlling this wallet (you must keep it secret), and my_wallet_id.addr is a (not so secret) file containing the address of this wallet. Once this is done, you have to transfer some (test) Grams to the non-bounceable address of your wallet, and run "sendfile my_wallet_id-query.boc" in the Lite Client to finish creating the new wallet. This process is explained in more detail in the LiteClient-HOWTO.

If you are running a validator in the "mainnet", it is a good idea to use more sophisticated wallet smart contracts (e.g., a multi-signature wallet). For the "testnet", the simple wallet should be enough.

3. Elector smart contract
~~~~~~~~~~~~~~~~~~~~~~~~~

The elector smart contract is a special smart contract residing in the masterchain. Its full address is -1:xxx..xxx, where -1 is the workchain identifier (-1 corresponds to the masterchain), and xxx..xxx is the hexadecimal representation of its 256-bit address inside the masterchain. In order to find out this address, you have to read the configuration parameter #1 from a recent state of the blockchain. This is easily done by means of the command `getconfig 1` in the Lite Client:

> getconfig 1
ConfigParam(1) = ( elector_addr:xA4C2C7C05B093D470DE2316DBA089FA0DD775FD9B1EBFC9DC9D04B498D3A2DDA)
x{A4C2C7C05B093D470DE2316DBA089FA0DD775FD9B1EBFC9DC9D04B498D3A2DDA}

In this case, the complete elector address is -1:A4C2C7C05B093D470DE2316DBA089FA0DD775FD9B1EBFC9DC9D04B498D3A2DDA

We assume familiarity with the Lite Client and that you know how to run it and how to obtain a global configuration file for it. Notice that the above command can be run in batch mode by using the '-c' command-line option of the Lite Client:

$ lite-client -C <global-config-file> -c 'getconfig 1'
...
ConfigParam(1) = ( elector_addr:xA4C2C7C05B093D470DE2316DBA089FA0DD775FD9B1EBFC9DC9D04B498D3A2DDA)
x{A4C2C7C05B093D470DE2316DBA089FA0DD775FD9B1EBFC9DC9D04B498D3A2DDA}

$

The elector smart contract has several uses. Most importantly, you can participate in validator elections or collect unfrozen stakes and bonuses by sending messages from the controlling smart contract of your validator to the elector smart contract. You can also learn about current validator elections and their participants by invoking the so-called "get-methods" of the elector smart contract.

Namely, running

> runmethod -1:A4C2C7C05B093D470DE2316DBA089FA0DD775FD9B1EBFC9DC9D04B498D3A2DDA active_election_id
...
arguments:  [ 86535 ] 
result:  [ 1567633899 ] 

(or lite-client -C <global-config> -c "runmethod -1:<elector-addr> active_election_id" in batch mode) will return the identifier of the currently active elections (a non-zero integer, typically the Unix time of the start of the service term of the validator group being elected), or 0, if no elections are currently active. In this example, the identifier of the active elections is 1567633899.

You can also recover the list of all active participants (pairs of 256-bit validator public keys and their corresponding stakes expressed in nanograms) by running the method "participant_list" instead of "active_election_id".

4. Creating a validator public key and ADNL address
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In order to participate in validator elections, you need to know the elections identifier (obtained by running get-method "active_elections_id" of the elector smart contract), and also the public key of your validator. The public key is created by running validator-engine-console (as explained in FullNode-HOWTO) and running the following commands:

$ validator-engine-console ...
...
conn ready
> newkey
created new key BCA335626726CF2E522D287B27E4FAFFF82D1D98615957DB8E224CB397B2EB67
> exportpub BCA335626726CF2E522D287B27E4FAFFF82D1D98615957DB8E224CB397B2EB67
got public key: xrQTSIQEsqZkWnoADMiBnyBFRUUweXTvzRQFqb5nHd5xmeE6
> addpermkey BCA335626726CF2E522D287B27E4FAFFF82D1D98615957DB8E224CB397B2EB67 1567633899 1567733900
success

Now the full node (validator-engine) has generated a new keypair, exported the base64 representation of the public key (xrQT...E6), and registered it as a persistent key for signing blocks starting from Unix time 1567633899 (equal to the election identifier) until 1567733900 (equal to the previous number plus the term duration of the validator set to be elected, available in configuration parameter #15, which can be learned by typing "getconfig 15" in the Lite Client, plus a safety margin in case elections actually happen later than intended).

You also need to define a temporary key to be used by the validator to participate in the network consensus protocol. The simplest way (sufficient for testing purposes) is to set this key equal to the persistent (block signing) key:

> addtempkey BCA335626726CF2E522D287B27E4FAFFF82D1D98615957DB8E224CB397B2EB67 BCA335626726CF2E522D287B27E4FAFFF82D1D98615957DB8E224CB397B2EB67 1567733900
success

It is also a good idea to create a dedicated ADNL address to be used exclusively for validator purposes:

> newkey
created new key C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C
> addadnl C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C 0
success
> addvalidatoraddr BCA335626726CF2E522D287B27E4FAFFF82D1D98615957DB8E224CB397B2EB67 C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C 1567733900
success

Now C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C is a new ADNL address, which will be used by the Full Node for running as a validator with the public key BCA...B67, with expiration time set to 1567733900.

5. Creating an election participation request
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The special script validator-elect-req.fif (located in <source-dir>/crypto/smartcont) is used to create a message that has to be signed by the validator in order to participate in the elections. It is run as follows:

$ fift -s validator-elect-req.fif <wallet-addr> <elect-utime> <max-factor> <adnl-addr> [<savefile>]

For example,

$ fift -s validator-elect-req.fif kf-vF9tD9Atqok5yA6n4yGUjEMiMElBi0RKf6IPqob1nY2dP 1567633899 2.7 C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C

or, if you have created the controlling wallet by means of new-wallet.fif, you can use @my_wallet_id.addr instead of copying the wallet address kf-vF...dP:

---------------------------------------
$ fift -s validator-elect-req.fif @my_wallet_id.addr 1567633899 2.7 C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C

Creating a request to participate in validator elections at time 1567633899 from smart contract Uf+vF9tD9Atqok5yA6n4yGUjEMiMElBi0RKf6IPqob1nY4EA = -1:af17db43f40b6aa24e7203a9f8c8652310c88c125062d1129fe883eaa1bd6763  with maximal stake factor with respect to the minimal stake 176947/65536 and validator ADNL address c5c2b94529405fb07d1ddfb4c42bfb07727e7ba07006b2db569fbf23060b9e5c 
654C50745D7031EB0002B333AF17DB43F40B6AA24E7203A9F8C8652310C88C125062D1129FE883EAA1BD6763C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C
ZUxQdF1wMesAArMzrxfbQ_QLaqJOcgOp-MhlIxDIjBJQYtESn-iD6qG9Z2PFwrlFKUBfsH0d37TEK_sHcn57oHAGsttWn78jBgueXA==
---------------------------------------

Here <max-factor> = 2.7 is the maximum ratio allowed between your stake and the minimal validator stake in the elected validator group. In this way you can be sure that your stake will be no more than 2.7 times the smallest stake, so the workload of your validator is at most 2.7 times the lowest one. If your stake is too large compared to the stakes of other validators, then it will be clipped to this value (2.7 times the smallest stake), and the remainder will be returned to you (i.e., to the controlling smart contract of your validator) immediately after elections.

Now you obtain a binary string in hexadecimal (654C...9E5C) and base64 form to be signed by the validator. This can be done in validator-engine-console:

> sign BCA335626726CF2E522D287B27E4FAFFF82D1D98615957DB8E224CB397B2EB67 654C50745D7031EB0002B333AF17DB43F40B6AA24E7203A9F8C8652310C88C125062D1129FE883EAA1BD6763C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C
got signature ovf9cmr2J/speJEtMU+tZm6zH/GBEyZCPpaukqL3mmNH9Wipyoys63VFh0yR386bARHKMPpfKAYBYslOjdSjCQ

Here BCA...B67 is the identifier of the signing key of our validator, and 654...E5C is the message generated by validator-elect-req.fif. The signature is ovf9...jCQ (this is the base64 representation of 64-byte Ed25519 signature).

Now you have to run another script validator-elect-signed.fif, which also requires the public key and the signature of the validator:

------------------------------------
$ fift -s validator-elect-signed.fif @my_wallet_id.addr 1567633899 2.7 C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C xrQTSIQEsqZkWnoADMiBnyBFRUUweXTvzRQFqb5nHd5xmeE6 ovf9cmr2J/speJEtMU+tZm6zH/GBEyZCPpaukqL3mmNH9Wipyoys63VFh0yR386bARHKMPpfKAYBYslOjdSjCQ==
Creating a request to participate in validator elections at time 1567633899 from smart contract Uf+vF9tD9Atqok5yA6n4yGUjEMiMElBi0RKf6IPqob1nY4EA = -1:af17db43f40b6aa24e7203a9f8c8652310c88c125062d1129fe883eaa1bd6763  with maximal stake factor with respect to the minimal stake 176947/65536 and validator ADNL address c5c2b94529405fb07d1ddfb4c42bfb07727e7ba07006b2db569fbf23060b9e5c 
String to sign is: 654C50745D7031EB0002B333AF17DB43F40B6AA24E7203A9F8C8652310C88C125062D1129FE883EAA1BD6763C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C
Provided a valid Ed25519 signature A2F7FD726AF627FB2978912D314FAD666EB31FF1811326423E96AE92A2F79A6347F568A9CA8CACEB7545874C91DFCE9B0111CA30FA5F28060162C94E8DD4A309 with validator public key 8404B2A6645A7A000CC8819F20454545307974EFCD1405A9BE671DDE7199E13A
query_id set to 1567632790 

Message body is x{4E73744B000000005D702D968404B2A6645A7A000CC8819F20454545307974EFCD1405A9BE671DDE7199E13A5D7031EB0002B333C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C}
 x{A2F7FD726AF627FB2978912D314FAD666EB31FF1811326423E96AE92A2F79A6347F568A9CA8CACEB7545874C91DFCE9B0111CA30FA5F28060162C94E8DD4A309}

Saved to file validator-query.boc
-----------------------

Alternatively, if you are running validator-engine-console on the same machine as your wallet, you can skip the above steps and instead use the `createelectionbid` command in the Validator Console to directly create a file (e.g., "validator-query.boc") with the message body containing your signed elections participation request. For this command to work, you have to run validator-engine with the `-f <fift-dir>` command-line option, where <fift-dir> is a directory containing copies of all required Fift source files (such as Fift.fif, TonUtil.fif, validator-elect-req.fif, and validator-elect-signed.fif), even though these files normally reside in different source directories (<source-dir>/crypto/fift/lib and <source-dir>/crypto/smartcont).

Now you have a message body containing your elections participation request. You must send it from the controlling smart contract, carrying the stake as its value (plus one extra Gram for sending confirmation). If you use the simple wallet smart contract, this can be done by using the `-B` command-line argument to wallet.fif:

--------------------------------------------
$ fift -s wallet.fif my_wallet_id -1:A4C2C7C05B093D470DE2316DBA089FA0DD775FD9B1EBFC9DC9D04B498D3A2DDA 1 100001. -B validator-query.boc 
Source wallet address = -1:af17db43f40b6aa24e7203a9f8c8652310c88c125062d1129fe883eaa1bd6763 
kf-vF9tD9Atqok5yA6n4yGUjEMiMElBi0RKf6IPqob1nY2dP
Loading private key from file my_wallet_id.pk
Transferring GR$100001. to account kf-kwsfAWwk9Rw3iMW26CJ-g3Xdf2bHr_J3J0EtJjTot2lHQ = -1:a4c2c7c05b093d470de2316dba089fa0dd775fd9b1ebfc9dc9d04b498d3a2dda seqno=0x1 bounce=-1 
Body of transfer message is x{4E73744B000000005D702D968404B2A6645A7A000CC8819F20454545307974EFCD1405A9BE671DDE7199E13A5D7031EB0002B333C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C}
 x{A2F7FD726AF627FB2978912D314FAD666EB31FF1811326423E96AE92A2F79A6347F568A9CA8CACEB7545874C91DFCE9B0111CA30FA5F28060162C94E8DD4A309}

signing message: x{0000000101}
 x{627FD26163E02D849EA386F118B6DD044FD06EBBAFECD8F5FE4EE4E825A4C69D16ED32D79A60A8500000000000000000000000000001}
  x{4E73744B000000005D702D968404B2A6645A7A000CC8819F20454545307974EFCD1405A9BE671DDE7199E13A5D7031EB0002B333C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C}
   x{A2F7FD726AF627FB2978912D314FAD666EB31FF1811326423E96AE92A2F79A6347F568A9CA8CACEB7545874C91DFCE9B0111CA30FA5F28060162C94E8DD4A309}

resulting external message: x{89FF5E2FB687E816D5449CE40753F190CA4621911824A0C5A2253FD107D5437ACEC6049CF8B8EA035B0446E232DB8C1DFEA97738076162B2E053513310D2A3A66A2A6C16294189F8D60A9E33D1E74518721B126A47DA3A813812959BD0BD607923B010000000080C_}
 x{627FD26163E02D849EA386F118B6DD044FD06EBBAFECD8F5FE4EE4E825A4C69D16ED32D79A60A8500000000000000000000000000001}
  x{4E73744B000000005D702D968404B2A6645A7A000CC8819F20454545307974EFCD1405A9BE671DDE7199E13A5D7031EB0002B333C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C}
   x{A2F7FD726AF627FB2978912D314FAD666EB31FF1811326423E96AE92A2F79A6347F568A9CA8CACEB7545874C91DFCE9B0111CA30FA5F28060162C94E8DD4A309}

B5EE9C7241040401000000013D0001CF89FF5E2FB687E816D5449CE40753F190CA4621911824A0C5A2253FD107D5437ACEC6049CF8B8EA035B0446E232DB8C1DFEA97738076162B2E053513310D2A3A66A2A6C16294189F8D60A9E33D1E74518721B126A47DA3A813812959BD0BD607923B010000000080C01016C627FD26163E02D849EA386F118B6DD044FD06EBBAFECD8F5FE4EE4E825A4C69D16ED32D79A60A85000000000000000000000000000010201A84E73744B000000005D702D968404B2A6645A7A000CC8819F20454545307974EFCD1405A9BE671DDE7199E13A5D7031EB0002B333C5C2B94529405FB07D1DDFB4C42BFB07727E7BA07006B2DB569FBF23060B9E5C030080A2F7FD726AF627FB2978912D314FAD666EB31FF1811326423E96AE92A2F79A6347F568A9CA8CACEB7545874C91DFCE9B0111CA30FA5F28060162C94E8DD4A309062A7721
(Saved to file wallet-query.boc)
----------------------------------

Now you just have to send wallet-query.boc from the Lite Client (not the Validator Console):

> sendfile wallet-query.boc

or you can use the Lite Client in batch mode:

$ lite-client -C <config-file> -c "sendfile wallet-query.boc"

This is an external message signed by your private key (which controls your wallet); it instructs your wallet smart contract to send an internal message to the elector smart contract with the prescribed payload (containing the validator bid and signed by its key) and transfer the specified amount of Grams. When the elector smart contract receives this internal message, it registers your bid (with the stake equal to the specified amount of Grams minus one), and sends you (i.e., the wallet smart contract) a confirmation (carrying 1 Gram minus message forwarding fees back) or a rejection message with an error code (carrying back almost all of the original stake amount minus processing fees).

You can check whether your stake has been accepted by running get-method "participant_list" of the elector smart contract.

6. Recovering stakes and bonuses
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If your stake is only partially accepted (because of <max-factor>) during the elections, or after your stake is unfrozen (this happens some time after the expiration of the term of the validator group to which your validator has been elected), you may want to collect back all or part of your stake, along with whatever share of bonuses is due to your validator. The elector smart contract does not send the stake and bonuses to you (i.e., the controlling smart contract) in a message. Instead, it credits the amount to be returned to you inside a special table, which can be inspected with the aid of get-method "compute_returned_stake" (which expects the address of the controlling smart contract as an argument):

$ lite-client -C global-config.json -rc 'runmethod -1:A4C2C7C05B093D470DE2316DBA089FA0DD775FD9B1EBFC9DC9D04B498D3A2DDA compute_returned_stake 0xaf17db43f40b6aa24e7203a9f8c8652310c88c125062d1129fe883eaa1bd6763'
arguments:  [ 79196899299028790296381692623119733846152089453039582491866112477478757689187 130944 ]
result: [ 0 ]

If the result is zero, nothing is due to you. Otherwise, you'll see part or all of your stake, perhaps with some bonuses. In that case, you can create a stake recovery request by using recover-stake.fif:

-----------------------------
$ fift -s recover-stake.fif
query_id for stake recovery message is set to 1567634299 

Message body is x{47657424000000005D70337B}

Saved to file recover-query.boc
-----------------------------

Again, you have to send recover-query.boc as the payload of a message from the controlling smart contract (i.e., your wallet) to the elector smart contract:

$ fift -s wallet.fif my_wallet_id <dest-addr> <my-wallet-seqno> <gram-amount> -B recover-query.boc

For example,

$ fift -s wallet.fif my_wallet_id -1:A4C2C7C05B093D470DE2316DBA089FA0DD775FD9B1EBFC9DC9D04B498D3A2DDA 2 1. -B recover-query.boc 
...
(Saved to file wallet-query.boc)

Notice that this message carries a small value (one Gram) just to pay the message forwarding and processing fees. If you indicate a value equal to zero, the message will not be processed by the election smart contract (a message with exactly zero value is almost useless in the TON Blockchain context).

Once wallet-query.boc is ready, you can send it from the Lite Client:

$ liteclient -C <config> -c 'sendfile wallet-query.boc'

If you have done everything correctly (in particular indicated the correct seqno of your wallet instead of "2" in the example above), you'll obtain a message from the elector smart contract containing the change from the small value you sent with your request (1. Gram in this example) plus the recovered portion of your stake and bonuses.

7. Participating in the next elections
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Notice that even before the term of the validator group containing your elected validator finishes, new elections for the next validator group will be announced. You'll probably want to participate in them as well. For this, you can use the same validator, but you must generate a new validator key and new ADNL address. You'll also have to make a new stake before your previous stake is returned (because your previous stake will be unfrozen and returned only some time after the next validator group becomes active), so if you want to participate in concurrent elections, it likely does not make sense to stake more than half of your Grams.
