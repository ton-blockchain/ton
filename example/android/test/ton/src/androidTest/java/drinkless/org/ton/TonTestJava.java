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
    String config = "{\n"+
        "  \"liteservers\": [\n"+
        "    {\n"+
        "      \"ip\": 1137658550,\n"+
        "      \"port\": 4924,\n"+
        "      \"id\": {\n"+
        "        \"@type\": \"pub.ed25519\",\n"+
        "        \"key\": \"peJTw/arlRfssgTuf9BMypJzqOi7SXEqSPSWiEw2U1M=\"\n"+
        "      }\n"+
        "    }\n"+
        "  ],\n"+
        "  \"validator\": {\n"+
        "    \"@type\": \"validator.config.global\",\n"+
        "    \"zero_state\": {\n"+
        "      \"workchain\": -1,\n"+
        "      \"shard\": -9223372036854775808,\n"+
        "      \"seqno\": 0,\n"+
        "      \"root_hash\": \"F6OpKZKqvqeFp6CQmFomXNMfMj2EnaUSOXN+Mh+wVWk=\",\n"+
        "      \"file_hash\": \"XplPz01CXAps5qeSWUtxcyBfdAo5zVb1N979KLSKD24=\"\n"+
        "    }\n"+
        "  }\n"+
        "}";

    private void appendLog(String log) {
        Log.w("XX", log);
    }

    @Test
    public void createTestWallet() {
        appendLog("start...");
        String dir =  getContext().getExternalFilesDir(null) + "/";
        String[] words = getContext().getString(R.string.wallet_mnemonic_words).split(" ");
        JavaClient client = new JavaClient();
        Object result = client.send(new TonApi.Init(new TonApi.Options(new TonApi.Config(config, "", false, false), new TonApi.KeyStoreTypeDirectory((dir)))));
        if (!(result instanceof TonApi.OptionsInfo)) {
            appendLog("failed to set config");
            return;
        }
        appendLog("config set ok");
        TonApi.OptionsInfo info = (TonApi.OptionsInfo)result;
        TonApi.Key key = (TonApi.Key) client.send(new TonApi.CreateNewKey("local password".getBytes(), "mnemonic password".getBytes(), "".getBytes()));
        TonApi.InputKey inputKey = new TonApi.InputKeyRegular(key, "local password".getBytes());
        TonApi.AccountAddress walletAddress = (TonApi.AccountAddress)client.send(new TonApi.GetAccountAddress(new TonApi.WalletV3InitialAccountState(key.publicKey, info.configInfo.defaultWalletId), 1));

        TonApi.Key giverKey = (TonApi.Key)client.send(new TonApi.ImportKey("local password".getBytes(), "".getBytes(), new TonApi.ExportedKey(words))) ;
        TonApi.InputKey giverInputKey = new TonApi.InputKeyRegular(giverKey, "local password".getBytes());
        TonApi.AccountAddress giverAddress = (TonApi.AccountAddress)client.send(new TonApi.GetAccountAddress(new TonApi.WalletV3InitialAccountState(giverKey.publicKey, info.configInfo.defaultWalletId), 1));

        appendLog("sending grams...");
        TonApi.QueryInfo queryInfo = (TonApi.QueryInfo)client.send(new TonApi.CreateQuery(giverInputKey, giverAddress, 60, new TonApi.ActionMsg(new TonApi.MsgMessage[]{new TonApi.MsgMessage(walletAddress, "", 6660000000L, new TonApi.MsgDataText("Hello".getBytes()) )}, true)));
        result = client.send(new TonApi.QuerySend(queryInfo.id));
        if (!(result instanceof TonApi.Ok)) {
            appendLog("failed to send grams");
            return;
        }
        appendLog("grams sent, getting balance");

        while (true) {
            TonApi.FullAccountState state = (TonApi.FullAccountState) client.send(new TonApi.GetAccountState(walletAddress));
            if (state.balance <= 0L) {
                try {
                    Thread.sleep(1000);
                } catch (Throwable e) {
                    appendLog(e.toString());
                }
            } else {
                appendLog(String.format("balance = %d", state.balance));
                break;
            }
        }
    }
}
