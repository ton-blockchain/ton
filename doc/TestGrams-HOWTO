The aim of this text is to describe how to quickly obtain a small amount of test Grams for test purposes, or a larger amount of test Grams for running a validator in the test network. We assume familiarity with the TON Blockchain LiteClient as explained in the LiteClient-HOWTO, and with the procedure required to compile the LiteClient and other software. For obtaining larger amount of test Grams required for running a validator, we also assume acquaintance with the FullNode-HOWTO and Validator-HOWTO. You will also need a dedicated server powerful enough for running a Full Node in order to obtain the larger amount of test Grams. Obtaining small amounts of test Grams does not require a dedicated server and may be done in several minutes on a home computer.

1. Proof-of-Work TestGiver smart contracts
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In order to prevent a small number of malicious parties to collect all test Grams reserved for test purposes, a special kind of "Proof-of-Work TestGiver" smart contracts have been deployed in the masterchain of the test network. The addresses of these smart contacts are:

Small testgivers (deliver from 10 to 100 test Grams every several minutes):

kf-kkdY_B7p-77TLn2hUhM6QidWrrsl8FYWCIvBMpZKprBtN
kf8SYc83pm5JkGt0p3TQRkuiM58O9Cr3waUtR9OoFq716lN-
kf-FV4QTxLl-7Ct3E6MqOtMt-RGXMxi27g4I645lw6MTWraV
kf_NSzfDJI1A3rOM0GQm7xsoUXHTgmdhN5-OrGD8uwL2JMvQ
kf8gf1PQy4u2kURl-Gz4LbS29eaN4sVdrVQkPO-JL80VhOe6
kf8kO6K6Qh6YM4ddjRYYlvVAK7IgyW8Zet-4ZvNrVsmQ4EOF
kf-P_TOdwcCh0AXHhBpICDMxStxHenWdLCDLNH5QcNpwMHJ8
kf91o4NNTryJ-Cw3sDGt9OTiafmETdVFUMvylQdFPoOxIsLm
kf9iWhwk9GwAXjtwKG-vN7rmXT3hLIT23RBY6KhVaynRrIK7
kf8JfFUEJhhpRW80_jqD7zzQteH6EBHOzxiOhygRhBdt4z2N

Large testgivers (deliver 10000 test Grams at least once a day):

kf8guqdIbY6kpMykR8WFeVGbZcP2iuBagXfnQuq0rGrxgE04
kf9CxReRyaGj0vpSH0gRZkOAitm_yDHvgiMGtmvG-ZTirrMC
kf-WXA4CX4lqyVlN4qItlQSWPFIy00NvO2BAydgC4CTeIUme
kf8yF4oXfIj7BZgkqXM6VsmDEgCqWVSKECO1pC0LXWl399Vx
kf9nNY69S3_heBBSUtpHRhIzjjqY0ChugeqbWcQGtGj-gQxO
kf_wUXx-l1Ehw0kfQRgFtWKO07B6WhSqcUQZNyh4Jmj8R4zL
kf_6keW5RniwNQYeq3DNWGcohKOwI85p-V2MsPk4v23tyO3I
kf_NSPpF4ZQ7mrPylwk-8XQQ1qFD5evLnx5_oZVNywzOjSfh
kf-uNWj4JmTJefr7IfjBSYQhFbd3JqtQ6cxuNIsJqDQ8SiEA
kf8mO4l6ZB_eaMn1OqjLRrrkiBcSt7kYTvJC_dzJLdpEDKxn

The first ten smart contracts enable a tester willing to obtain a small amount of test Grams to obtain some without spending too much computing power (typically, several minutes of work of a home computer should suffice). The remaining smart contracts are for obtaining larger amounts of test Grams required for running a validator in the test network; typically, a day of work of a dedicated server powerful enough to run a validator should suffice to obtain the necessary amount.

You should randomly choose one of these "proof-of-work testgiver" smart contracts (from one of these two lists depending on your purpose) and obtain test Grams from this smart contract by a procedure similar to "mining". Essentially, you have to present an external message containing the proof of work and the address of your wallet to the chosen "proof-of-work testgiver" smart contract, and then the necessary amount will be sent to you.

2. The "mining" process
~~~~~~~~~~~~~~~~~~~~~~~

In order to create an external message containing the "proof of work", you should run a special "mining" utility, compiled from the TON sources located in the GitHub repository. The utility is located in file './crypto/pow-miner' with respect to the build directory, and can be compiled by typing 'make pow-miner' in the build directory.

However, before running "pow-miner", you need to know the actual values of "seed" and "complexity" parameters of the chosen "proof-of-work testgiver" smart contract. This can be done by invoking get-method "get_pow_params" of this smart contract. For instance, if you use testgiver smart contract kf-kkdY_B7p-77TLn2hUhM6QidWrrsl8FYWCIvBMpZKprBtN, you can simply type

    > runmethod kf-kkdY_B7p-77TLn2hUhM6QidWrrsl8FYWCIvBMpZKprBtN get_pow_params

in the LiteClient console and obtain output like

    ...
    arguments:  [ 101616 ] 
    result:  [ 229760179690128740373110445116482216837 53919893334301279589334030174039261347274288845081144962207220498432 100000000000 256 ] 
    remote result (not to be trusted):  [ 229760179690128740373110445116482216837 53919893334301279589334030174039261347274288845081144962207220498432 100000000000 256 ] 

The two first large numbers in the "result:" line are the "seed" and the "complexity" of this smart contract. In this example, the seed is 229760179690128740373110445116482216837 and the complexity is 53919893334301279589334030174039261347274288845081144962207220498432.

Next, you invoke the pow-miner utility as follows:

      $ crypto/pow-miner -vv -w<num-threads> -t<timeout-in-sec> <your-wallet-address> <seed> <complexity> <iterations> <pow-testgiver-address> <boc-filename>

Here <num-threads> is the number of CPU cores that you want to use for mining, <timeout-in-sec> is the maximal amount of seconds that the miner would run before admitting failure, <your-wallet-address> is the address of your wallet (possibly not initialized yet), either in the masterchain or in the workchain (note that you need a masterchain wallet to control a validator), <seed> and <complexity> are the most recent values obtained by running get-method 'get-pow-params', <pow-testgiver-address> is the address of the chosen proof-of-work testgiver smartcontract, and <boc-filename> is the filename of the output file where the external message with the proof of work will be saved in the case of success.

For example, if your wallet address is kQBWkNKqzCAwA9vjMwRmg7aY75Rf8lByPA9zKXoqGkHi8SM7, you might run

    $ crypto/pow-miner -vv -w7 -t100 kQBWkNKqzCAwA9vjMwRmg7aY75Rf8lByPA9zKXoqGkHi8SM7 229760179690128740373110445116482216837 53919893334301279589334030174039261347274288845081144962207220498432 100000000000 kf-kkdY_B7p-77TLn2hUhM6QidWrrsl8FYWCIvBMpZKprBtN mined.boc

The program will run for some time (at most 100 seconds in this case) and either terminate successfully (with zero exit code) and save the required proof of work into file "mined.boc", or terminate with a non-zero exit code if no proof of work was found.

In the case of failure, you will see something like

   [ expected required hashes for success: 2147483648 ]
   [ hashes computed: 1192230912 ]

and the program will terminate with a non-zero exit code. Then you have to obtain the "seed" and "complexity" again (because they may have changed in the meantime as a result of processing requests from more successful miners) and re-run the "pow-miner" with the new parameters, repeating the process again and again until success.

In the case of success, you will see something like

   [ expected required hashes for success: 2147483648 ]
   4D696E65005EFE49705690D2AACC203003DBE333046683B698EF945FF250723C0F73297A2A1A41E2F1A1F533B3BC4F5664D6C743C1C5C74BB3342F3A7314364B3D0DA698E6C80C1EA4ACDA33755876665780BAE9BE8A4D6385A1F533B3BC4F5664D6C743C1C5C74BB3342F3A7314364B3D0DA698E6C80C1EA4
   Saving 176 bytes of serialized external message into file `mined.boc`
   [ hashes computed: 1122036095 ]

Then you can use the LiteClient to send external message from file "mined.boc" to the proof-of-work testgiver smart contract (and you must do this as soon as possible):

> sendfile mined.boc
...	external message status is 1

You can wait for several seconds and check the state of your wallet:

> last
> getaccount kQBWkNKqzCAwA9vjMwRmg7aY75Rf8lByPA9zKXoqGkHi8SM7
...
account state is (account
  addr:(addr_std
    anycast:nothing workchain_id:0 address:x5690D2AACC203003DBE333046683B698EF945FF250723C0F73297A2A1A41E2F1)
  storage_stat:(storage_info
    used:(storage_used
      cells:(var_uint len:1 value:1)
      bits:(var_uint len:1 value:111)
      public_cells:(var_uint len:0 value:0)) last_paid:1593722498
    due_payment:nothing)
  storage:(account_storage last_trans_lt:7720869000002
    balance:(currencies
      grams:(nanograms
        amount:(var_uint len:5 value:100000000000))
      other:(extra_currencies
        dict:hme_empty))
    state:account_uninit))
x{C005690D2AACC203003DBE333046683B698EF945FF250723C0F73297A2A1A41E2F12025BC2F7F2341000001C169E9DCD0945D21DBA0004_}
last transaction lt = 7720869000001 hash = 83C15CDED025970FEF7521206E82D2396B462AADB962C7E1F4283D88A0FAB7D4
account balance is 100000000000ng

If nobody has sent a valid proof of work with this *seed* and *complexity* before you, the proof-of-work testgiver will accept your proof of work and this will be reflected in the balance of your wallet (10 or 20 seconds may elapse after sending the external message before this happens; be sure to make several attempts and type "last" each time before checking the balance of your wallet to refresh the LiteClient state). In the case of success, you will see that the balance has been increased (and even that your wallet has been created in uninitialized state if it did not exist before). In the case of failure, you will have to obtain the new "seed" and "complexity" and repeat the mining process from the very beginning.

If you have been lucky and the balance of your wallet has been increased, you may want to initialize the wallet if it wasn't initialized before (more information on wallet creation can be found in LiteClient-HOWTO):

> sendfile new-wallet-query.boc
...	external message status is 1
> last
> getaccount kQBWkNKqzCAwA9vjMwRmg7aY75Rf8lByPA9zKXoqGkHi8SM7
...
account state is (account
  addr:(addr_std
    anycast:nothing workchain_id:0 address:x5690D2AACC203003DBE333046683B698EF945FF250723C0F73297A2A1A41E2F1)
  storage_stat:(storage_info
    used:(storage_used
      cells:(var_uint len:1 value:3)
      bits:(var_uint len:2 value:1147)
      public_cells:(var_uint len:0 value:0)) last_paid:1593722691
    due_payment:nothing)
  storage:(account_storage last_trans_lt:7720945000002
    balance:(currencies
      grams:(nanograms
        amount:(var_uint len:5 value:99995640998))
      other:(extra_currencies
        dict:hme_empty))
    state:(account_active
      (
        split_depth:nothing
        special:nothing
        code:(just
          value:(raw@^Cell 
            x{}
             x{FF0020DD2082014C97BA218201339CBAB19C71B0ED44D0D31FD70BFFE304E0A4F260810200D71820D70B1FED44D0D31FD3FFD15112BAF2A122F901541044F910F2A2F80001D31F3120D74A96D307D402FB00DED1A4C8CB1FCBFFC9ED54}
            ))
        data:(just
          value:(raw@^Cell 
            x{}
             x{00000001CE6A50A6E9467C32671667F8C00C5086FC8D62E5645652BED7A80DF634487715}
            ))
        library:hme_empty))))
x{C005690D2AACC203003DBE333046683B698EF945FF250723C0F73297A2A1A41E2F1206811EC2F7F23A1800001C16B0BC790945D20D1929934_}
 x{FF0020DD2082014C97BA218201339CBAB19C71B0ED44D0D31FD70BFFE304E0A4F260810200D71820D70B1FED44D0D31FD3FFD15112BAF2A122F901541044F910F2A2F80001D31F3120D74A96D307D402FB00DED1A4C8CB1FCBFFC9ED54}
 x{00000001CE6A50A6E9467C32671667F8C00C5086FC8D62E5645652BED7A80DF634487715}
last transaction lt = 7720945000001 hash = 73353151859661AB0202EA5D92FF409747F201D10F1E52BD0CBB93E1201676BF
account balance is 99995640998ng

Now you are a happy owner of 100 test Grams that can be used for whatever testing purposes you had in mind. Congratulations!

3. Automating the mining process in the case of failure
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If you fail to obtain your test Grams for a long time, this may happen because too many other testers are simultaneously "mining" from the same proof-of-work testgiver smart contract. Maybe you should choose another proof-of-work testgiver smart contract from one of the lists given above. Alternatively, you can write a simple script to automatically run `pow-miner` with the correct parameters again and again until success (detected by checking the exit code of `pow-miner`) and invoke the lite-client with parameter -c 'sendfile mined.boc' to send the external message immediately after it is found.

