package drinkless.org.ton;


import java.util.concurrent.CountDownLatch;

import android.support.test.runner.AndroidJUnit4;
import android.util.Log;
import drinkless.org.ton.Client;
import drinkless.org.ton.TonApi;
import org.junit.Test;
import org.junit.runner.RunWith;

import static android.support.test.InstrumentationRegistry.getContext;

@RunWith(AndroidJUnit4.class)
public class TonTestJava {
    class JavaClient {
        Client client = Client.create(null, null, null);

        public Object send(TonApi.Function query) {
            Object[] result = new Object[1];
            CountDownLatch countDownLatch = new CountDownLatch(1);

            class Callback implements Client.ResultHandler {
                Object[] result;
                CountDownLatch countDownLatch;

                Callback(Object[] result, CountDownLatch countDownLatch) {
                    this.result = result;
                    this.countDownLatch = countDownLatch;
                }

                public void onResult(TonApi.Object object) {
                    if (object instanceof TonApi.Error) {
                        appendLog(((TonApi.Error) object).message);
                    } else {
                        result[0] = object;
                    }
                    if (countDownLatch != null) {
                        countDownLatch.countDown();
                    }
                }
            }

            client.send(query, new Callback(result, countDownLatch) , null);
            if (countDownLatch != null) {
                try {
                    countDownLatch.await();
                } catch (Throwable e) {
                    appendLog(e.toString());
                }
            }
            return result[0];
        }
    }

    String config = "{\n" +
            "  \"liteservers\": [\n" +
            "    {\n" +
            "      \"ip\": 1137658550,\n" +
            "      \"port\": 4924,\n" +
            "      \"id\": {\n" +
            "        \"@type\": \"pub.ed25519\",\n" +
            "        \"key\": \"peJTw/arlRfssgTuf9BMypJzqOi7SXEqSPSWiEw2U1M=\"\n" +
            "      }\n" +
            "    }\n" +
            "  ],\n" +
            "  \"validator\": {\n" +
            "    \"@type\": \"validator.config.global\",\n" +
            "    \"zero_state\": {\n" +
            "      \"workchain\": -1,\n" +
            "      \"shard\": -9223372036854775808,\n" +
            "      \"seqno\": 0,\n" +
            "      \"root_hash\": \"VCSXxDHhTALFxReyTZRd8E4Ya3ySOmpOWAS4rBX9XBY=\",\n" +
            "      \"file_hash\": \"eh9yveSz1qMdJ7mOsO+I+H77jkLr9NpAuEkoJuseXBo=\"\n" +
            "    }\n" +
            "  }\n" +
            "}";

    private void appendLog(String log) {
        Log.w("XX", log);
    }

    @Test
    public void createTestWallet() {
        appendLog("start...");

            JavaClient client = new JavaClient();
            Object result = client.send(new TonApi.Init(new TonApi.Options(new TonApi.Config(config, "", false, false), getContext().getExternalFilesDir(null) + "/")));
            if (!(result instanceof TonApi.Ok)) {
                appendLog("failed to set config");
                return;
            }
            appendLog("config set ok");
            TonApi.Key key = (TonApi.Key) client.send(new TonApi.CreateNewKey("local password".getBytes(), "mnemonic password".getBytes(), "".getBytes()));
            appendLog("got private key");
            TonApi.AccountAddress walletAddress = (TonApi.AccountAddress) client.send(new TonApi.TestWalletGetAccountAddress(new TonApi.TestWalletInitialAccountState(key.publicKey)));
            appendLog("got account address");
            appendLog("sending grams...");
            TonApi.TestGiverAccountState testGiverState = (TonApi.TestGiverAccountState) client.send(new TonApi.TestGiverGetAccountState());
            result = client.send(new TonApi.TestGiverSendGrams(walletAddress, testGiverState.seqno, 6660000000L, "".getBytes()));
            if (!(result instanceof TonApi.Ok)) {
                appendLog("failed to send grams");
                return;
            }
            appendLog("grams sent, getting balance");

            while (true) {
                TonApi.GenericAccountStateUninited accountStateUninited = (TonApi.GenericAccountStateUninited) client.send(new TonApi.GenericGetAccountState(walletAddress));
                if (accountStateUninited == null || accountStateUninited.accountState.balance <= 0L) {
                    try {
                        Thread.sleep(1000);
                    } catch (Throwable e) {
                        appendLog(e.toString());
                    }
                } else {
                    appendLog(String.format("balance = %d", accountStateUninited.accountState.balance));
                    break;
                }
            }

            TonApi.InputKey inputKey = new TonApi.InputKey(key, "local password".getBytes());
            result = client.send(new TonApi.TestWalletInit(inputKey));
            if (!(result instanceof TonApi.Ok)) {
                return;
            }
            appendLog("init test wallet ok, getting state...");

            while (true) {
                TonApi.GenericAccountState accountState = (TonApi.GenericAccountState) client.send(new TonApi.GenericGetAccountState(walletAddress));
                if (!(accountState instanceof TonApi.GenericAccountStateTestWallet)) {
                    try {
                        Thread.sleep(1000);
                    } catch (Throwable e) {
                        appendLog(e.toString());
                    }
                } else {
                    appendLog("got account state");
                    break;
                }
            }

            appendLog("sending grams...");
            TonApi.GenericAccountStateTestWallet state = (TonApi.GenericAccountStateTestWallet) client.send(new TonApi.GenericGetAccountState(walletAddress));
            long balance = state.accountState.balance;
            result = client.send(new TonApi.GenericSendGrams(inputKey, walletAddress, walletAddress, 10, 0, true, "hello".getBytes()));
            if (!(result instanceof TonApi.Ok)) {
                return;
            }
            appendLog(String.format("grams sent, current balance %d, receving...", balance));

            while (true) {
                TonApi.GenericAccountStateTestWallet wallet = (TonApi.GenericAccountStateTestWallet) client.send(new TonApi.GenericGetAccountState(walletAddress));
                if (wallet == null || wallet.accountState.balance == balance) {
                    try {
                        Thread.sleep(1000);
                    } catch (Throwable e) {
                        appendLog(e.toString());
                    }
                } else {
                    appendLog(String.format("grams received, balance = %d", balance));
                    break;
                }
            }
        appendLog("OK");

    }
}
