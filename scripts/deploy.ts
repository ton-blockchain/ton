/**
 * Template deploy script for HighloadWalletV3 (keypair-based sender)
 *
 * Behavior:
 * - Loads compiled code cell from wrappers/compiled.ts
 * - Builds data cell using highloadWalletV3ConfigToCell
 * - Computes the contract address and prints it
 * - Determines deploy amount:
 *    - If DEPLOY_VALUE env var is set, use it
 *    - Otherwise try to call a TonClient estimate function (if available at runtime)
 *    - If estimation fails, fallback to DEPLOY_FALLBACK (default 0.1 TON)
 *    - Apply DEPLOY_SAFE_MARGIN (default 1.5)
 * - Prints the chosen amount and exits unless CONFIRM_DEPLOY=true
 * - The actual SEND DEPLOY block is marked with TODO: implement using your wallet/provider
 *
 * Safety:
 * - Does not send funds unless CONFIRM_DEPLOY=true to avoid accidental deploys from CI
 * - Does not contain any secret key; expects SECRET_KEY_BASE64 or a secret manager to be used
 */

import 'dotenv/config';
import { toNano, contractAddress } from '@ton/core';
import { HighloadWalletV3Code } from '../wrappers/compiled';
import { highloadWalletV3ConfigToCell } from '../wrappers/HighloadWalletV3';

const RPC_URL = process.env.RPC_URL || 'https://net.ton.dev';
const SECRET_KEY_BASE64 = process.env.SECRET_KEY_BASE64 || '';
const WORKCHAIN = Number(process.env.WORKCHAIN || 0);

// Default subwallet id for HighloadWalletV3 as per the HighloadWalletV3 specification.
// Can be overridden via the SUBWALLET_ID environment variable if needed.
const HIGHLOAD_WALLET_V3_DEFAULT_SUBWALLET_ID = 0x10ad;

const SUBWALLET_ID = Number(process.env.SUBWALLET_ID || HIGHLOAD_WALLET_V3_DEFAULT_SUBWALLET_ID);
const TIMEOUT = Number(process.env.TIMEOUT || 3600);
const DEPLOY_VALUE_ENV = process.env.DEPLOY_VALUE || '';
const DEPLOY_SAFE_MARGIN = Number(process.env.DEPLOY_SAFE_MARGIN || '1.5');
const DEPLOY_FALLBACK = process.env.DEPLOY_FALLBACK || '0.1';
const CONFIRM_DEPLOY = process.env.CONFIRM_DEPLOY === 'true'; // must be explicitly set to "true" to send

async function estimateDeployAmount(init: any): Promise<bigint> {
    // If user provided explicit value, use it
    if (DEPLOY_VALUE_ENV) {
        console.log('Using DEPLOY_VALUE from env:', DEPLOY_VALUE_ENV);
        return toNano(DEPLOY_VALUE_ENV);
    }

    // Try to use TonClient dynamically (if installed and available at runtime)
    try {
        // dynamic require to avoid TypeScript type errors if library evolves
        // eslint-disable-next-line @typescript-eslint/no-var-requires
        const { TonClient } = require('ton');
        const client = new TonClient({ endpoint: RPC_URL });

        // Try some common estimate API names in a safe way
        if (typeof client.estimateFeesForDeploy === 'function') {
            console.log('Estimating deploy fees with TonClient.estimateFeesForDeploy...');
            const res = await client.estimateFeesForDeploy({ init });
            const fee = BigInt(res?.totalFee ?? res?.fee ?? 0);
            const withMargin = BigInt(Math.ceil(Number(fee) * DEPLOY_SAFE_MARGIN));
            console.log('Estimated fee:', fee.toString(), 'with margin:', withMargin.toString());
            return withMargin;
        }

        if (typeof client.estimateFees === 'function') {
            console.log('Estimating deploy fees with TonClient.estimateFees...');
            const res = await client.estimateFees({ init });
            const fee = BigInt(res?.total ?? 0);
            const withMargin = BigInt(Math.ceil(Number(fee) * DEPLOY_SAFE_MARGIN));
            console.log('Estimated fee:', fee.toString(), 'with margin:', withMargin.toString());
            return withMargin;
        }

        console.warn('TonClient found but no known estimate method present. Falling back.');
    } catch (e) {
        console.warn(
            'Could not use TonClient to estimate fees (skipped). Error:',
            e instanceof Error ? e.message : String(e),
        );
    }

    // Fallback to default conservative amount
    console.log('Using fallback deploy value:', DEPLOY_FALLBACK, 'TON');
    const fallback = toNano(DEPLOY_FALLBACK);
    const withMargin = BigInt(Math.ceil(Number(fallback) * DEPLOY_SAFE_MARGIN));
    return withMargin;
}

async function main() {
    console.log('RPC URL:', RPC_URL);
    console.log('Workchain:', WORKCHAIN);

    if (!SECRET_KEY_BASE64) {
        console.warn('SECRET_KEY_BASE64 is not set. You will need to provide your secret key before sending real deploys. The script will still compute address and estimate amounts.');
    }

    // TODO: replace this placeholder with deriving the actual publicKey from your secret key
    // For now we use a zeroed publicKey buffer as placeholder to compute address deterministically
    const publicKeyPlaceholder = Buffer.alloc(32, 0);

    const data = highloadWalletV3ConfigToCell({
        publicKey: publicKeyPlaceholder,
        subwalletId: SUBWALLET_ID,
        timeout: TIMEOUT,
    });

    const init = { code: HighloadWalletV3Code, data };
    const address = contractAddress(WORKCHAIN, init);

    console.log('Computed contract address:', address.toString({ urlSafe: true, bounceable: true }));

    const deployAmount = await estimateDeployAmount(init);
    console.log('Resolved deploy amount (nano-tons):', deployAmount.toString());
    console.log('In TON (approx):', (Number(deployAmount) / 1e9).toFixed(9));

    if (!CONFIRM_DEPLOY) {
        console.log('CONFIRM_DEPLOY is not set to true. Exiting before any network action.');
        console.log('Set CONFIRM_DEPLOY=true and ensure SECRET_KEY_BASE64 is set to actually send the deploy.');
        return;
    }

    // =====================
    // TODO: SEND DEPLOY MESSAGE (KEYPAIR-BASED, Option A)
    // =====================
    // The following steps must be implemented using your chosen wallet/send approach:
    // 1) Derive your keypair (publicKey, secretKey) from SECRET_KEY_BASE64 using the same crypto lib
    // 2) Create a wallet/sender object or build an internal message that contains init (code+data)
    // 3) Sign the message appropriately and send it to the network using a TonClient or your RPC provider
    // 4) Wait for transaction confirmation and verify contract activation (code+data present)
    //
    // Example guidance (pseudocode):
    // const key = Buffer.from(SECRET_KEY_BASE64, 'base64');
    // const keyPair = deriveKeyPair(key);
    // const client = new TonClient({ endpoint: RPC_URL });
    // const sender = createSenderFromKey(keyPair);
    // await highloadContract.sendDeploy(provider, sender, deployAmount);
    //
    // Important: do not commit secret keys in repo. Use secrets or a secure vault.

    console.log('CONFIRM_DEPLOY was true but SEND DEPLOY is not implemented. Implement the TODO block to perform the actual deploy.');
}

main().catch((e) => {
    console.error('Error:', e);
    process.exit(1);
});
