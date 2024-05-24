package drinkless.org.ton

import android.content.Context
import android.support.test.InstrumentationRegistry
import android.support.test.InstrumentationRegistry.getContext
import android.support.test.filters.LargeTest
import android.support.test.filters.SmallTest
import android.support.test.runner.AndroidJUnit4
import drinkless.org.ton.Client
import drinkless.org.ton.TonApi

import org.junit.Test
import org.junit.runner.RunWith

import org.junit.Assert.*
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.coroutines.suspendCoroutine
import kotlinx.coroutines.*;

class ClientKotlin {
    val client = Client.create(null, null, null);
    suspend fun send(query: TonApi.Function) : TonApi.Object {
        return suspendCoroutine<TonApi.Object> {  cont ->
            client.send(query, {
                cont.resume(it)
            },null)
        }
    }
}

@RunWith(AndroidJUnit4::class)
@SmallTest
class TonTest {
    val config = """{
  "liteservers": [
    {
      "ip": 1137658550,
      "port": 4924,
      "id": {
        "@type": "pub.ed25519",
        "key": "peJTw/arlRfssgTuf9BMypJzqOi7SXEqSPSWiEw2U1M="
      }
    }
  ],
  "validator": {
    "@type": "validator.config.global",
    "zero_state": {
      "workchain": -1,
      "shard": -9223372036854775808,
      "seqno": 0,
      "root_hash": "F6OpKZKqvqeFp6CQmFomXNMfMj2EnaUSOXN+Mh+wVWk=",
      "file_hash": "XplPz01CXAps5qeSWUtxcyBfdAo5zVb1N979KLSKD24="
    }
  }
}"""
    @Test
    fun createTestWallet() {
        val client = ClientKotlin()
        val dir = getContext().getExternalFilesDir(null).toString() + "/";
        val words = getContext().getString(R.string.wallet_mnemonic_words).split(" ").toTypedArray();
        runBlocking {
            val info = client.send(TonApi.Init(TonApi.Options(TonApi.Config(config, "", false, false), TonApi.KeyStoreTypeDirectory(dir)))) as TonApi.OptionsInfo;
            val key = client.send(TonApi.CreateNewKey("local password".toByteArray(), "mnemonic password".toByteArray(), "".toByteArray())) as TonApi.Key
            val inputKey = TonApi.InputKeyRegular(key, "local password".toByteArray())
            val walletAddress = client.send(TonApi.GetAccountAddress(TonApi.WalletV3InitialAccountState(key.publicKey, info.configInfo.defaultWalletId), 1)) as TonApi.AccountAddress

            val giverKey = client.send(TonApi.ImportKey("local password".toByteArray(), "".toByteArray(), TonApi.ExportedKey(words))) as TonApi.Key
            val giverInputKey = TonApi.InputKeyRegular(giverKey, "local password".toByteArray())
            val giverAddress = client.send(TonApi.GetAccountAddress(TonApi.WalletV3InitialAccountState(giverKey.publicKey, info.configInfo.defaultWalletId), 1)) as TonApi.AccountAddress;

            val queryInfo = client.send(TonApi.CreateQuery(giverInputKey, giverAddress, 60, TonApi.ActionMsg(arrayOf(TonApi.MsgMessage(walletAddress, inputKey.key.publicKey, 6660000000, TonApi.MsgDataDecryptedText("Helo".toByteArray()) )), true))) as TonApi.QueryInfo;
            client.send(TonApi.QuerySend(queryInfo.id)) as TonApi.Ok;

            while ((client.send(TonApi.GetAccountState(walletAddress)) as TonApi.FullAccountState).balance <= 0L) {
                delay(1000L)
            }

            val queryInfo2 = client.send(TonApi.CreateQuery(inputKey, walletAddress, 60, TonApi.ActionMsg(arrayOf(), true))) as TonApi.QueryInfo;
            client.send(TonApi.QuerySend(queryInfo2.id)) as TonApi.Ok;
            while ((client.send(TonApi.GetAccountState(walletAddress)) as TonApi.FullAccountState).accountState !is TonApi.WalletV3AccountState) {
                delay(1000L)
            }
        }
    }
}

