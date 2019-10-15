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
      "root_hash": "VCSXxDHhTALFxReyTZRd8E4Ya3ySOmpOWAS4rBX9XBY=",
      "file_hash": "eh9yveSz1qMdJ7mOsO+I+H77jkLr9NpAuEkoJuseXBo="
    }
  }
}"""
    @Test
    fun createTestWallet() {
        val client = ClientKotlin()
        val dir = getContext().getExternalFilesDir(null).toString() + "/";
        runBlocking {
            client.send(TonApi.Init(TonApi.Options(TonApi.Config(config, "", false, false), dir)))
            val key = client.send(TonApi.CreateNewKey("local password".toByteArray(), "mnemonic password".toByteArray(), "".toByteArray())) as TonApi.Key
            val walletAddress = client.send(TonApi.TestWalletGetAccountAddress(TonApi.TestWalletInitialAccountState(key.publicKey))) as TonApi.AccountAddress;
            val testGiverState = client.send(TonApi.TestGiverGetAccountState()) as TonApi.TestGiverAccountState

            client.send(TonApi.TestGiverSendGrams(walletAddress, testGiverState.seqno, 6660000000, "".toByteArray())) as TonApi.Ok

            while ((client.send(TonApi.GenericGetAccountState(walletAddress)) as TonApi.GenericAccountStateUninited).accountState.balance <= 0L) {
                delay(1000L)
            }

            val inputKey = TonApi.InputKey(key, "local password".toByteArray());
            client.send(TonApi.TestWalletInit(inputKey)) as TonApi.Ok

            while (client.send(TonApi.GenericGetAccountState(walletAddress)) !is TonApi.GenericAccountStateTestWallet) {
                delay(1000L)
            }

            val state = client.send(TonApi.GenericGetAccountState(walletAddress)) as TonApi.GenericAccountStateTestWallet
            val balance = state.accountState.balance
            client.send(TonApi.GenericSendGrams(inputKey, walletAddress, walletAddress, 10, 0, true, "hello".toByteArray())) as TonApi.Ok
            while ((client.send(TonApi.GenericGetAccountState(walletAddress)) as TonApi.GenericAccountStateTestWallet).accountState.balance == balance) {
                delay(1000L)
            }
        }
    }
}

