// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <miner.h>

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/tx_verify.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <hash.h>
#include <validation.h>
#include <net.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <timedata.h>
#include <txmempool.h>
#include <util.h>
#include <utilmoneystr.h>
#include <validationinterface.h>

#include <rpc/mining.h>
#include <rpc/safemode.h>
#include <rpc/server.h>
#include <rpc/blockchain.h>
#include <rpc/rawtransaction.h>
#include <core_io.h>

#include <algorithm>
#include <queue>
#include <utility>

#include <boost/scope_exit.hpp>
#include <contract_storage/contract_storage.hpp>
#include "txdb.h"
#include "wallet/wallet.h"
#include "base58.h"
#include <univalue.h>


//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest fee rate of a transaction combined with all
// its ancestors.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockWeight = 0;
uint64_t nLastBlockSize = 0;
int64_t posSleepTime = 0;

posState posstate;

extern CAmount nMinimumInputValue;
extern CAmount nReserveBalance;
//extern int nStakeMinConfirmations;

static bool CheckKernel(CBlock* pblock, const COutPoint& prevout, CAmount amount,int nHeight);
//static bool CheckKernel(CBlock* pblock, const COutPoint& prevout, CAmount amount, int32_t utxoDepth);


int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);

    return nNewTime - nOldTime;
}

BlockAssembler::Options::Options() {
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
}

BlockAssembler::BlockAssembler(const CChainParams& params, const Options& options) : chainparams(params)
{
    int height = 0;
    {
        LOCK(cs_main);
        height = chainActive.Height();
    }
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit weight to between 4K and MaxBlockSize-4K for sanity:
    unsigned int nAbsMaxSize = MaxBlockSize(height + 1);
    nBlockMaxWeight = std::max<size_t>(4000, std::min<size_t>(nAbsMaxSize - 4000, options.nBlockMaxWeight));
	nBlockMaxSize = MaxBlockSize(height+1);;
}

static BlockAssembler::Options DefaultOptions(const CChainParams& params)
{
    // Block resource limits
    // If neither -blockmaxsize or -blockmaxweight is given, limit to DEFAULT_BLOCK_MAX_*
    // If only one is given, only restrict the specified resource.
    // If both are given, restrict both.
    BlockAssembler::Options options;
    options.nBlockMaxWeight = gArgs.GetArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
    if (gArgs.IsArgSet("-blockmintxfee")) {
        CAmount n = 0;
        ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n);
        options.blockMinFeeRate = CFeeRate(n);
    } else {
        options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    }
    return options;
}

BlockAssembler::BlockAssembler(const CChainParams& params) : BlockAssembler(params, DefaultOptions(params)) {}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockSize = 1000;
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;
    fIncludeWitness = false;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, bool fMineWitnessTx, int64_t* pTotalFees, int32_t txProofTime, int32_t nTimeLimit)
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return nullptr;
    pblock = &pblocktemplate->block; // pointer for convenience

    this->nTimeLimit = nTimeLimit;

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

    LOCK2(cs_main, mempool.cs);
    CBlockIndex* pindexPrev = chainActive.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;
	nBlockMaxWeight = std::min(nBlockMaxWeight, MaxBlockSize(nHeight));

    pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus(),MINING_TYPE_POW);
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);

    pblock->nTime = GetAdjustedTime();
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                       ? nMedianTimePast
                       : pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization) or when
    // -promiscuousmempoolflags is used.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = IsWitnessEnabled(pindexPrev, chainparams.GetConsensus()) && fMineWitnessTx;

    int64_t nTime1 = GetTimeMicros();

    nLastBlockTx = nBlockTx;
    nLastBlockSize = nBlockSize;
    nLastBlockWeight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
    coinbaseTx.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    originalRewardTx = coinbaseTx;
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));

    //////////////////////////////////////////////////////// contract
	auto allow_contract = nHeight >= Params().GetConsensus().UBCONTRACT_Height;

    minGasPrice = DEFAULT_MIN_GAS_PRICE;
    hardBlockGasLimit = DEFAULT_BLOCK_GAS_LIMIT;
    softBlockGasLimit = hardBlockGasLimit;
    softBlockGasLimit = std::min(softBlockGasLimit, hardBlockGasLimit);
    txGasLimit = softBlockGasLimit;

    nBlockMaxSize = MaxBlockSerSize;

	// save old root state hash
	std::shared_ptr<::contract::storage::ContractStorageService> service;
	std::string old_root_state_hash;
    bool rollbacked_contract_storage = false;
	if (allow_contract) {
		service = get_contract_storage_service();
		service->open();
		old_root_state_hash = service->current_root_state_hash();
		service->close();
	}
    BOOST_SCOPE_EXIT_ALL(&) {
        if(allow_contract && !rollbacked_contract_storage) {
            service->open();
            service->rollback_contract_state(old_root_state_hash);
            rollbacked_contract_storage = true;
            service->close();
        }
    };
    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    if(nHeight != Params().GetConsensus().ForkV4Height && nHeight != Params().GetConsensus().ForkV5Height)
        addPackageTxs(nPackagesSelected, nDescendantsUpdated, minGasPrice, allow_contract);

	if(allow_contract)
		service->open();

    if(allow_contract) {
        const auto &root_state_hash_after_add_txs = service->current_root_state_hash();
		CTxOut root_state_hash_out;
		root_state_hash_out.scriptPubKey =
			CScript() << ValtypeUtils::string_to_vch(root_state_hash_after_add_txs) << OP_ROOT_STATE_HASH;
		root_state_hash_out.nValue = 0;
		CMutableTransaction txCoinBaseToChange(*(pblock->vtx[0]));
		txCoinBaseToChange.vout.push_back(root_state_hash_out);
		originalRewardTx = txCoinBaseToChange;
        pblock->vtx[0] = MakeTransactionRef(std::move(txCoinBaseToChange));
        pblocktemplate->vchCoinbaseRootStateHash =
                std::vector<unsigned char>(root_state_hash_out.scriptPubKey.begin(),
                                           root_state_hash_out.scriptPubKey.end());
    }

    
    if(nHeight == Params().GetConsensus().ForkV4Height)
    {
        std::vector<std::pair<COutPoint, CTxOut>> outputs;
        GetBadUTXO(outputs);
        LogPrintf("GetBadUTXO(outputs): %d\n", outputs.size());
        for(const auto& output:outputs)
        {
            LogPrintf("findOutPut,badoutput: %s,badn: %d\n", output.first.hash.ToString().c_str(),output.first.n);
        }
        std::vector<CTransactionRef> vtx;
        CreateHolyTransactions(outputs,vtx);
        std::vector<CTransactionRef>::iterator it;
        for(it = vtx.begin();it!=vtx.end();++it)
        {
            pblock->vtx.push_back(*it);
        }
    }

    if(nHeight == Params().GetConsensus().ForkV5Height)
    {
        std::vector<CTransactionRef> vtx;
        CreateRefundTx(vtx);
        pblock->vtx.push_back(vtx[0]);
    }

	// rollback root state hash
	if (allow_contract) {
		service->rollback_contract_state(old_root_state_hash);
        rollbacked_contract_storage = true;
		service->close();
	}

	RebuildRefundTransaction();

    pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus());
    pblocktemplate->vTxFees[0] = -nFees;

    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);
    LogPrintf("%s\n", pblock->ToString());

    // The total fee is the Fees minus the Refund
    if (pTotalFees) {
        *pTotalFees = nFees;
    }

    // The total fee is the Fees minus the Refund
    if (pTotalFees) {
        *pTotalFees = nFees;
    }

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    pblock->nNonce         = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    CValidationState state;
    if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n", 0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1), 0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlockPos(CWalletRef& pwallet, int32_t nTimeLimit, bool fMineWitnessTx)
{
    //int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return nullptr;
    pblock = &pblocktemplate->block; // pointer for convenience

	this->nTimeLimit = nTimeLimit;

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1);

    LOCK2(cs_main, mempool.cs);

    if (!EnsureWalletIsAvailable(pwallet, true))
        return nullptr;

    if(chainActive.Height()+1 <(Params().GetConsensus().UBCONTRACT_Height))
    	return nullptr;

    //std::shared_ptr<CReserveScript> coinbase_script;
    //pwallet->GetScriptForMining(coinbase_script);

    // If the keypool is exhausted, no script is returned at all.  Catch this.
    //if (!coinbase_script)
    //    return nullptr;	

    CBlockIndex* pindexPrev = chainActive.Tip();
    nHeight = pindexPrev->nHeight + 1;
    if(nHeight == Params().GetConsensus().ForkV4Height || nHeight == Params().GetConsensus().ForkV5Height)
        return nullptr;

    pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus(), MINING_TYPE_POS);
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);

    pblock->nTime = GetAdjustedTime();
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                       ? nMedianTimePast
                       : pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization) or when
    // -promiscuousmempoolflags is used.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = IsWitnessEnabled(pindexPrev, chainparams.GetConsensus()) && fMineWitnessTx;

    /*int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated, minGasPrice, true);*/

    //int64_t nTime1 = GetTimeMicros();

    nLastBlockTx = nBlockTx;
    nLastBlockSize = nBlockSize;
    nLastBlockWeight = nBlockWeight;
    
    pblocktemplate->vTxFees[0] = -nFees;

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    pblock->nNonce         = 0;

	// Create coin stake
	CTransaction txCoinStake;
	txCoinStake.vin.clear();
	txCoinStake.vout.clear();
	// Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    //txCoinStake.vout.push_back(CTxOut(0, scriptEmpty));
    posstate.numOfUtxo = 0;
    posstate.sumOfutxo = 0;

	// Choose coins to use
    CAmount nBalance = pwallet->GetBalance();
    if (nBalance <= nReserveBalance) {
    	//LogPrintf("CreateNewBlockPos(): nBalance not enough for POS, less than nReserveBalance\n");
        return nullptr;
    }

    std::set<std::pair<const CWalletTx*,unsigned int> > setCoins;
    int64_t nValueIn = 0;

    // Select coins with suitable depth
    if (!pwallet->SelectCoinsForStaking(nBalance - nReserveBalance, setCoins, nValueIn))
        return nullptr;

    posstate.numOfUtxo = setCoins.size();
    posstate.sumOfutxo = nValueIn;

    if (setCoins.empty())
        return nullptr;

	int64_t nCredit = 0;
	bool fKernelFound = false;
	CScript scriptPubKeyKernel;
	COutPoint prevoutFound;
    int64_t startTime=0;
    int64_t endTime=0;
    startTime = GetTimeMillis();

	for (const auto& pcoin: setCoins) {
		COutPoint prevoutStake = COutPoint(pcoin.first->GetHash(), pcoin.second);

		Coin coinStake;
		if (!pcoinsTip->GetCoin(prevoutStake, coinStake)) {
			//return nullptr;
			continue;
		}	

		//CTxOut vout = pcoin.first->tx->vout[pcoin.second];
		//int nDepth = pcoin.first->GetDepthInMainChain();

		//if (CheckKernel(pblock, prevoutStake, vout.nValue, nDepth)) {
		if (CheckKernel(pblock, prevoutStake, coinStake.out.nValue,nHeight)) {
            // Found a kernel
            LogPrintf("CreateCoinStake : kernel found\n");

			// Set prevoutFound
			prevoutFound = prevoutStake;
		
            std::vector<std::vector<unsigned char> > vSolutions;
            txnouttype whichType;
            CScript scriptPubKeyOut;
			//scriptPubKeyKernel = vout.scriptPubKey;
            scriptPubKeyKernel = coinStake.out.scriptPubKey;
            if (!Solver(scriptPubKeyKernel, whichType, vSolutions))  {
                LogPrintf("CreateNewBlockPos(): failed to parse kernel\n");
                break;
            }
            LogPrintf("CreateNewBlockPos(): parsed kernel type=%d\n", whichType);
            if (whichType != TX_SCRIPTHASH  && 
				whichType != TX_MULTISIG  &&
				whichType != TX_PUBKEYHASH && 
				whichType != TX_PUBKEY && 
				whichType != TX_WITNESS_V0_SCRIPTHASH &&
				whichType != TX_WITNESS_V0_KEYHASH) {
                LogPrintf("CreateNewBlockPos(): no support for kernel type=%d\n", whichType);
                break;  
            }
            if (whichType == TX_SCRIPTHASH || 
				whichType == TX_MULTISIG || 
				whichType == TX_PUBKEYHASH || 
				whichType == TX_PUBKEY || 
				whichType == TX_WITNESS_V0_SCRIPTHASH || 
				whichType == TX_WITNESS_V0_KEYHASH) {
				// use the same script pubkey
                scriptPubKeyOut = scriptPubKeyKernel;
            }

			// push empty vin
            txCoinStake.vin.push_back(CTxIn(prevoutStake));
            nCredit += coinStake.out.nValue;
            //nCredit += vout.nValue;
			// push empty vout
			CTxOut empty_txout = CTxOut();
			empty_txout.SetEmpty();
			txCoinStake.vout.push_back(empty_txout);
            txCoinStake.vout.push_back(CTxOut(nCredit, scriptPubKeyOut));

            LogPrintf("CreateNewBlockPos(): added kernel type=%d\n", whichType);
            fKernelFound = true;
            break;
        }
	}
    endTime  = GetTimeMillis();
    posSleepTime = endTime - startTime;

	if (!fKernelFound)
		return nullptr;

    if (nCredit == 0 || nCredit > nBalance - nReserveBalance)
        return nullptr;
	txCoinStake.hash = txCoinStake.ComputeHash();

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
	// reward to pos miner 1 coin 
    coinbaseTx.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    //coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
	// nExtraNonce
    int nExtraNonce = 1;
    coinbaseTx.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(coinbaseTx.vin[0].scriptSig.size() <= 100);
	// specify vout scriptpubkey of coinbase transaction (the first transaction)
	coinbaseTx.vout[0].scriptPubKey =  scriptPubKeyKernel;
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));


    //////////////////////////////////////////////////////// contract
    auto allow_contract = nHeight >= Params().GetConsensus().UBCONTRACT_Height;

    minGasPrice = DEFAULT_MIN_GAS_PRICE;
    hardBlockGasLimit = DEFAULT_BLOCK_GAS_LIMIT;
    softBlockGasLimit = hardBlockGasLimit;
    softBlockGasLimit = std::min(softBlockGasLimit, hardBlockGasLimit);
    txGasLimit = softBlockGasLimit;

    nBlockMaxSize = MaxBlockSerSize;

    // save old root state hash
    std::shared_ptr<::contract::storage::ContractStorageService> service;
    std::string old_root_state_hash;
    bool rollbacked_contract_storage = false;
    if (allow_contract) {
        service = get_contract_storage_service();
        service->open();
        old_root_state_hash = service->current_root_state_hash();
        service->close();
    }
    BOOST_SCOPE_EXIT_ALL(&) {
        if(allow_contract && !rollbacked_contract_storage) {
            service->rollback_contract_state(old_root_state_hash);
            rollbacked_contract_storage = true;
            service->close();
        }
    };
    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;

    addPackageTxs(nPackagesSelected, nDescendantsUpdated, minGasPrice, allow_contract, prevoutFound);

    if(allow_contract)
        service->open();

    if(allow_contract) {
        const auto &root_state_hash_after_add_txs = service->current_root_state_hash();
        CMutableTransaction txCoinbase(*pblock->vtx[0]);
        CTxOut tout;
        tout.nValue = 0;
        tout.scriptPubKey = CScript() << ValtypeUtils::string_to_vch(root_state_hash_after_add_txs) << OP_ROOT_STATE_HASH;
        txCoinbase.vout.push_back(tout);
        pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
        pblocktemplate->vchCoinbaseRootStateHash =
                std::vector<unsigned char>(tout.scriptPubKey.begin(),
                                           tout.scriptPubKey.end());
    }

    // rollback root state hash
    if (allow_contract) {
        service->rollback_contract_state(old_root_state_hash);
        rollbacked_contract_storage = true;
        service->close();
    }

    RebuildRefundTransaction();
    ////////////////////////////////////////////////////////

	// insert CoinStake
	pblock->vtx.insert(pblock->vtx.begin() + 1, MakeTransactionRef(std::move(txCoinStake)));
    
	pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);
	pblocktemplate->vTxSigOpsCost[1] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[1]);
	pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus());
	
	pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
	
	CValidationState state;
	if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
		throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
	}

    return std::move(pblocktemplate);
}

void BlockAssembler::RebuildRefundTransaction() {
	CMutableTransaction contrTx(*(pblock->vtx[0]));
	if(contrTx.vin.size()>0)
        contrTx.vin[0].prevout.SetNull();
	contrTx.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
	pblock->vtx[0] = MakeTransactionRef(std::move(contrTx));
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        }
        else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight)
        return false;
    if (nBlockSigOpsCost + packageSigOpsCost >= MaxBlockSigops(nHeight))
        return false;
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package)
{
    for (CTxMemPool::txiter it : package) {
        CValidationState state;
        if (!ContextualCheckTransaction(it->GetTx(), state,
                                        chainparams.GetConsensus(), nHeight,
                                        nLockTimeCutoff))
            return false;
        if (!fIncludeWitness && it->GetTx().HasWitness())
            return false;
    }
    return true;
}

bool BlockAssembler::AttemptToAddContractToBlock(CTxMemPool::txiter iter, uint64_t minGasPrice) {
    if (nTimeLimit != 0 && GetAdjustedTime() >= nTimeLimit - BYTECODE_TIME_BUFFER) {
        return false;
    }
    // operate on local vars first, then later apply to `this`
    uint64_t nBlockWeight = this->nBlockWeight;
    uint64_t nBlockSize = this->nBlockSize;
    uint64_t nBlockSigOpsCost = this->nBlockSigOpsCost;
    ContractTxConverter convert(iter->GetTx(), nullptr, &pblock->vtx);
    ExtractContractTX resultConverter;
    std::string error_ret;
    if (!convert.extractionContractTransactions(resultConverter, error_ret)) {
        //this check already happens when accepting txs into mempool
        //therefore, this can only be triggered by using raw transactions on the staker itself
        return false;
    }
    auto service = get_contract_storage_service();
    service->open();

    std::vector<ContractTransaction> contractTransactions = resultConverter.txs;
	CAmount sumGasCoins = 0;
	CAmount gasCountAllTxs = 0;
	uint64_t blockGasLimit = UINT64_MAX;
    uint64_t allDepositAmount = 0;
    uint64_t allWithdrawFromContractAmount = 0;
    for(auto& withdrawInfo : resultConverter.contract_withdraw_infos) {
        allWithdrawFromContractAmount += withdrawInfo.amount;
    }
	std::string error_str;
    for (const auto& contractTransaction : contractTransactions) {
		if (!contractTransaction.is_params_valid(service, -1, sumGasCoins, gasCountAllTxs, blockGasLimit, error_str)) {
			return false;
		}
		sumGasCoins += contractTransaction.params.gasLimit * contractTransaction.params.gasPrice;
		gasCountAllTxs += contractTransaction.params.gasLimit;
        allDepositAmount += contractTransaction.params.deposit_amount;
    }
	// check tx fee can over gas
    CAmount nTxFee = 0;
	{
		CCoinsViewCache view(pcoinsTip.get());
		nTxFee = view.GetValueIn(iter->GetTx()) + allWithdrawFromContractAmount - iter->GetTx().GetValueOut();
        if(nTxFee <= allDepositAmount)
            return false;
        nTxFee -= allDepositAmount;
		if (nTxFee < sumGasCoins)
			return false;
	}

	const auto& old_root_state_hash = service->current_root_state_hash();
    ContractExec exec(service.get(), *pblock, contractTransactions, hardBlockGasLimit, nTxFee);
	bool success = false;
	BOOST_SCOPE_EXIT_ALL(&service, &success, &old_root_state_hash) {
		if(!success)
			service->rollback_contract_state(old_root_state_hash);
	};

    if (!exec.performByteCode()) {
        //error, don't add contract
        return false;
    }
    ContractExecResult testExecResult;
    if (!exec.processingResults(testExecResult)) {
        return false;
    }
    if (bceResult.usedGas + testExecResult.usedGas > softBlockGasLimit) {
        //if this transaction could cause block gas limit to be exceeded, then don't add it
        return false;
    }
    // check withdraw-from-info correct
    if(!testExecResult.match_contract_withdraw_infos(resultConverter.contract_withdraw_infos)) {
        return false;
    }
    // commit changes than can generate new root state hash
    if(!exec.commit_changes(service))
        return false;

    //apply contractTx costs to local state
    if (fNeedSizeAccounting) {
        nBlockSize += ::GetSerializeSize(iter->GetTx(), SER_NETWORK, PROTOCOL_VERSION);
    }
    nBlockWeight += iter->GetTxWeight();
    nBlockSigOpsCost += iter->GetSigOpCost();
    //calculate sigops from new refund/proof tx
    //first, subtract old proof tx
    nBlockSigOpsCost -= GetLegacySigOpCount(*pblock->vtx[0]);
    // manually rebuild refundtx
    CMutableTransaction contrTx(*pblock->vtx[0]);

    nBlockSigOpsCost += GetLegacySigOpCount(contrTx);
    //all contract costs now applied to local state
    //Check if block will be too big or too expensive with this contract execution
    if (nBlockSigOpsCost * WITNESS_SCALE_FACTOR > (uint64_t)MAX_BLOCK_SIGOPS_COST ||
        nBlockSize > MaxBlockSerSize) {
        //contract will not be added to block
        return false;
    }
    //block is not too big, so apply the contract execution and it's results to the actual block
    //apply local bytecode to global bytecode state
    bceResult.usedGas += testExecResult.usedGas;
    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    if (fNeedSizeAccounting) {
        this->nBlockSize += ::GetSerializeSize(iter->GetTx(), SER_NETWORK, PROTOCOL_VERSION);
    }
    this->nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    this->nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);
    //calculate sigops from new refund/proof tx
    this->nBlockSigOpsCost -= GetLegacySigOpCount(*pblock->vtx[0]);
	RebuildRefundTransaction();
    this->nBlockSigOpsCost += GetLegacySigOpCount(*pblock->vtx[0]);
	
	success = true;
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries& alreadyAdded,
        indexed_modified_transaction_set &mapModifiedTx)
{
    int nDescendantsUpdated = 0;
    for (const CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc))
                continue;
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCostWithAncestors -= it->GetSigOpCost();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx, CTxMemPool::setEntries &failedTx)
{
    assert (it != mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, CTxMemPool::txiter entry, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.

void BlockAssembler::addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated, uint64_t minGasPrice, bool allow_contract, const COutPoint& outpointPos)
{
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    auto mi = mempool.mapTx.get<ancestor_score_or_gas_price>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != mempool.mapTx.get<ancestor_score_or_gas_price>().end() || !mapModifiedTx.empty())
    {
        if (nTimeLimit != 0 && GetAdjustedTime() >= nTimeLimit) {
            //no more time to add transactions, just exit
            return;
        }
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != mempool.mapTx.get<ancestor_score_or_gas_price>().end() &&
                SkipMapTxEntry(mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score_or_gas_price>().begin();
        if (mi == mempool.mapTx.get<ancestor_score_or_gas_price>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score_or_gas_price>().end()) {
				if (CompareModifiedEntry()(*modit, CTxMemPoolModifiedEntry(iter))) {
					// The best entry in mapModifiedTx has higher score
					// than the one from mapTx.
					// Switch which transaction (package) to consider
					iter = modit->iter;
					fUsingModified = true;
				}
				else {
					// it's worse than mapTx
					// Increment mi for the next loop iteration.
					++mi;
				}
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                failedTx.insert(iter);
            }
			continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, iter, sortedEntries);

        bool wasAdded = true;
        for (size_t i = 0; i<sortedEntries.size(); ++i) {
            if (!wasAdded || (nTimeLimit != 0 && GetAdjustedTime() >= nTimeLimit))
            {
                //if out of time, or earlier ancestor failed, then skip the rest of the transactions
                mapModifiedTx.erase(sortedEntries[i]);
                wasAdded = false;
                continue;
            }
            const CTransaction& tx = sortedEntries[i]->GetTx();

			// check UTXO spent by pos mining
			bool spentByPos = false;
			if (outpointPos.n != uint32_t(-1)) {
				for (const auto& vin : tx.vin) {
					if (vin.prevout == outpointPos) {
						spentByPos = true;
						break;
					}
				}
				
				if (spentByPos)
					continue;
			}
            if (wasAdded) {
				if (!allow_contract && (tx.HasContractOp() || tx.HasOpSpend())) {
					mapModifiedTx.erase(sortedEntries[i]);
					wasAdded = false;
					continue;
				}
                if (tx.HasContractOp()) {
                    wasAdded = AttemptToAddContractToBlock(sortedEntries[i], minGasPrice);
                    if (!wasAdded) {
                        if (fUsingModified) {
                            //this only needs to be done once to mark the whole package (everything in sortedEntries) as failed
                            mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                            failedTx.insert(iter);
                        }
                    }
                }
                else {
                    AddToBlock(sortedEntries[i]);
                }
            }
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

		if (!wasAdded) {
			//skip UpdatePackages if a transaction failed to be added (match TestPackage logic)
			continue;
		}

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}


bool CheckKernel(CBlock* pblock, const COutPoint& prevout, CAmount amount,int nHeight)
{
	Coin coinStake;
	if (!pcoinsTip->GetCoin(prevout, coinStake))
		return false;	

	int utxoHeight = coinStake.nHeight;
	
	if (utxoHeight > nHeight - Params().GetConsensus().nStakeMinConfirmations)
		return false;

    posstate.ifPos = 2;
    return CheckProofOfStake(pblock, prevout, amount, nHeight-utxoHeight);
}


bool CheckProofOfStake(CBlock* pblock, const COutPoint& prevout,  CAmount amount, int coinAge)
{
    int nHeight = 0;
    int nHeightPre10Blcok = 0;

    uint256 hashPrevBlock = pblock->hashPrevBlock;
	if (hashPrevBlock != uint256()) 
	{
        nHeight =  mapBlockIndex[hashPrevBlock]->nHeight;
        nHeightPre10Blcok = nHeight / 10 * 10;
    }
    

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(pblock->nBits);
    uint256 targetProofOfStake = ArithToUint256(bnTarget);

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    if ((nHeight + 1) < Params().GetConsensus().ForkV3Height)
    {
	    ss << pblock->nTime << prevout.hash << prevout.n;
	}
	else
	{
	    uint256 hashPrev10Block = pblock->hashPrevBlock;
        CBlockIndex* pblockindex = mapBlockIndex[hashPrev10Block];
        while(pblockindex)
        {        
            if(pblockindex->nHeight == nHeightPre10Blcok)
            {
                hashPrev10Block = pblockindex->GetBlockHash();
                break;
            }
            else
                pblockindex = pblockindex->pprev;
        }
	    ss << pblock->nTime << prevout.hash << prevout.n << hashPrev10Block;
	}
	uint256	hashProofOfStake = Hash(ss.begin(), ss.end());

	arith_uint256 bnHashPos = UintToArith256(hashProofOfStake);
	bnHashPos /= amount;
	//bnHashPos /= coinAge;

	uint256 hashProofOfStakeWeight = ArithToUint256(bnHashPos);
	//LogPrintf("CheckProofOfStake amount: %lld\n", amount);
	//LogPrintf("CheckProofOfStake coinAge: %d\n", coinAge);
	//LogPrintf("CheckProofOfStake bnTarget: %s\n", targetProofOfStake.ToString().c_str());
	//LogPrintf("CheckProofOfStake hashProofOfStake: %s\n", hashProofOfStake.ToString().c_str());
	//LogPrintf("CheckProofOfStake hashProofOfStakeWeight: %s\n", hashProofOfStakeWeight.ToString().c_str());

    if (bnHashPos > bnTarget)
        return false;

	return true;
}


bool CheckStake(CBlock* pblock)
{
    uint256 proofHash;
	uint256 hashTarget;
    uint256 hashBlock = pblock->GetHash();
    int nHeight = 0;
    if(!pblock->IsProofOfStake())
        return error("CheckStake() : %s is not a proof-of-stake block", hashBlock.GetHex());

    uint256 hashPrevBlock = pblock->hashPrevBlock;
    if (hashPrevBlock != uint256()) 
    {
        nHeight =  mapBlockIndex[hashPrevBlock]->nHeight;
    }

    //{
        //LOCK(cs_main);
		//nHeight = chainActive.Height();
    //}
	if ((nHeight + 1) < Params().GetConsensus().UBCONTRACT_Height)
		return error("CheckStake(): pos not allow at the current block height");

    // verify hash target and signature of coinstake tx
	// Check coin stake transaction
    if (!pblock->vtx[1]->IsCoinStake())
        return error("CheckStake() : called on non-coinstake %s", pblock->vtx[1]->GetHash().ToString());

	Coin coinStake;
	{
        LOCK2(cs_main,mempool.cs);
	    if (!pcoinsTip->GetCoin(pblock->vtx[1]->vin[0].prevout, coinStake))
		    return error("CheckStake() : can not get coinstake coin");
    }

	// Check stake min confirmations
	if (coinStake.nHeight > (nHeight + 1) - Params().GetConsensus().nStakeMinConfirmations)
		return error("CheckStake() : utxo can not reach stake min confirmations");

	if (!CheckProofOfStake(pblock, pblock->vtx[1]->vin[0].prevout, coinStake.out.nValue, (nHeight + 1)-coinStake.nHeight))
		return error("CheckStake() CheckProofOfStake");

	// Check pos authority
	CScript coinStakeFrom = coinStake.out.scriptPubKey;
	CScript coinStakeTo = pblock->vtx[1]->vout[1].scriptPubKey;
	
    txnouttype whichTypeFrom, whichTypeTo;
	std::vector<CTxDestination> txDestFromVec, txDestToVec;
	int nRequiredFrom, nRequiredTo;
	if (!ExtractDestinations(coinStakeFrom, whichTypeFrom, txDestFromVec, nRequiredFrom))
		return error("CheckStake() : ExtractDestinations coinStakeFrom ");

	if (!ExtractDestinations(coinStakeTo, whichTypeTo, txDestToVec, nRequiredTo))
		return error("CheckStake() : ExtractDestinations coinStakeTo ");

	if (whichTypeFrom != TX_SCRIPTHASH && whichTypeFrom != TX_MULTISIG &&
		whichTypeFrom != TX_PUBKEYHASH && whichTypeFrom != TX_PUBKEY &&
		whichTypeFrom != TX_WITNESS_V0_SCRIPTHASH && whichTypeFrom != TX_WITNESS_V0_KEYHASH)
		return error("CheckStake() : whichTypeFrom ");

	if (whichTypeTo != TX_SCRIPTHASH && whichTypeTo != TX_MULTISIG &&
		whichTypeTo != TX_PUBKEYHASH && whichTypeTo != TX_PUBKEY &&
		whichTypeTo != TX_WITNESS_V0_SCRIPTHASH && whichTypeTo != TX_WITNESS_V0_KEYHASH)
		return error("CheckStake() : whichTypeTo ");

	if (coinStakeFrom != coinStakeTo)
		return error("CheckStake() : coinStakeFrom != coinStakeTo");

	// Check stake value
	CAmount nValueFrom = coinStake.out.nValue;
	CAmount nValueTo = pblock->vtx[1]->vout[1].nValue;
	if (nValueFrom != nValueTo)
		return error("CheckStake() : nValueFrom != nValueTo ");

    //// debug print
    //LogPrintf("CheckStake() : new proof-of-stake block found  \n  hash: %s \nproofhash: %s  \ntarget: %s\n", hashBlock.GetHex(), proofHash.GetHex(), hashTarget.GetHex());
    //LogPrintf("%s\n", pblock->ToString());
    //LogPrintf("out %s\n", FormatMoney(pblock->vtx[1]->GetValueOut()));

    return true;
}

int GetHolyCoin(std::map<COutPoint, CAmount>& coins)
{
    unsigned int iStartBlockHeight = 750000;

    for(unsigned int iHeight = iStartBlockHeight;iHeight < Params().GetConsensus().ForkV4Height;iHeight++)
    {
        CBlockIndex* pblockindex = chainActive[iHeight];
        //uint256 blockHash = pblockindex->GetBlockHash();
        CBlock tmpBlock;
        if (!ReadBlockFromDisk(tmpBlock, pblockindex, Params().GetConsensus()))
        {
            return 0;
        }
        for(auto& tx: tmpBlock.vtx)
        {
            if(tx->IsCoinBase())
            {
                COutPoint outpoint;
                outpoint.hash = tx->GetHash();
                outpoint.n = 0;
                coins[outpoint]=tx->vout[0].nValue;
            }
            else if(tx->IsCoinStake())
            {
                COutPoint outpoint;
                outpoint.hash = tx->GetHash();
                outpoint.n = 1;
                coins[outpoint]=tx->vout[1].nValue;
            }
            else
            {
                for(int i=0;i<tx->vout.size();i++)
                {
                    COutPoint outpoint;
                    outpoint.hash = tx->GetHash();
                    outpoint.n = i;
                    coins[outpoint]=tx->vout[i].nValue;
                }
            }
            
        }
    }
    
}


int GetBadUTXO(std::vector<std::pair<COutPoint, CTxOut>>& outputs)
{
    unsigned int iStartBlockHeight = Params().GetConsensus().SCANBADTX_Height;
    //CCoinsViewCache coins(pcoinsTip.get());
    std::vector<std::string> whiteAddr={"3BbKnVAatHjjzXb8uSa3SyEFCYdUA6VMy9","1BycBHJvoSbfmsprK6QctGU7ei8MB4kAme"};
    std::map<COutPoint, CAmount> coins;
    
    GetHolyCoin(coins);

    for(unsigned int iHeight = iStartBlockHeight;iHeight < Params().GetConsensus().ForkV4Height;iHeight++)
    {
        CBlockIndex* pblockindex = chainActive[iHeight];
        //uint256 blockHash = pblockindex->GetBlockHash();
        CBlock tmpBlock;
        if (!ReadBlockFromDisk(tmpBlock, pblockindex, Params().GetConsensus()))
        {
            return 0;
        }
        for(auto& tx: tmpBlock.vtx)
        {
            bool bRelated = false;
            if(tmpBlock.IsProofOfStake() && tx->IsCoinStake())
            {
                COutPoint prevout = tx->vin[0].prevout;
                auto it = coins.find(prevout);
                CAmount value_in;
                if (it != coins.end()) {
                    value_in = it->second;
                }
                else
                    continue;
                CAmount value_out = tx->GetValueOut();
                if(value_in != value_out)
                {
                
                    //tmpTxid.push_back(tx.GetHash().GetHex()+"I"+std::to_string(1));
                    //tmpTxid.push_back( tmpBlock.vtx[0].GetHash().GetHex()++"I"+std::to_string(0));
                    	
                    // coin stake
                    COutPoint outpoint;
                    outpoint.hash = tx->GetHash();
                    outpoint.n = 1;
                    
                    CTxOut txout;
                    txout.nValue = tx->vout[1].nValue;
                    txout.scriptPubKey = tx->vout[1].scriptPubKey; 

                    auto it=outputs.end();
                    if(!findOutPut(outputs,outpoint,it))
                        outputs.emplace_back(std::make_pair(outpoint, txout));
                    
                    // coin base
                    COutPoint outpoint2;
                    outpoint2.hash = tmpBlock.vtx[0]->GetHash();
                    outpoint2.n = 0;
                    
                    CTxOut txout2;
                    txout2.nValue = tmpBlock.vtx[0]->vout[0].nValue;
                    txout2.scriptPubKey = tmpBlock.vtx[0]->vout[0].scriptPubKey; 
                    
                    auto it2=outputs.end();

                    if(!findOutPut(outputs,outpoint2,it2))
                        outputs.emplace_back(std::make_pair(outpoint2, txout2));
                }
                        
            }
            
            if(!tx->IsCoinBase())
            {
               for(unsigned int txi = 0;txi < tx->vin.size();txi++ )
               {
                    COutPoint outpoint;
                    outpoint.hash = tx->vin[txi].prevout.hash;
                    outpoint.n = tx->vin[txi].prevout.n;
                      
                    auto it=outputs.end();
                    findOutPut(outputs,outpoint,it);
                    if (it!=outputs.end())
                    {
                        bRelated = true;
                        outputs.erase(it);
                    }
               }
               
               if(bRelated == true)
               {
                  unsigned int  i=0;
                  if(tmpBlock.IsProofOfStake() && tx->IsCoinStake())
                  {
                  	//tmpTxid.push_back( tmpBlock.vtx[0].GetHash().GetHex()++"I"+std::to_string(0));
                      
                    // coin base
                    COutPoint outpoint2;
                    outpoint2.hash = tmpBlock.vtx[0]->GetHash();
                    outpoint2.n = 0;
                    
                    CTxOut txout2;
                    txout2.nValue = tmpBlock.vtx[0]->vout[0].nValue;
                    txout2.scriptPubKey = tmpBlock.vtx[0]->vout[0].scriptPubKey; 
                    auto it=outputs.end();
                    if(!findOutPut(outputs,outpoint2,it))
                        outputs.emplace_back(std::make_pair(outpoint2, txout2));
                    i =1;
                  }	
               	
               	
                  for(unsigned int txo = i;txo < tx->vout.size();txo++ )
                  {
                      txnouttype type;
                      std::string tmpAddr;
                      std::vector<CTxDestination> addresses;
                      int nRequired;
                      if (!ExtractDestinations(tx->vout[txo].scriptPubKey, type, addresses, nRequired)) {
                            LogPrintf("ExtractDestinations failed.\n");
                      }
                      tmpAddr = EncodeDestination(addresses[0]);
                      //LogPrintf("ExtractDestinations addresses:%s.\n",tmpAddr);
                      auto it=find(whiteAddr.begin(),whiteAddr.end(),tmpAddr);
                      if (it==whiteAddr.end())
                      {                                                       
                        COutPoint outpoint;
	                    outpoint.hash = tx->GetHash();
	                    outpoint.n = txo;
	                    
	                    CTxOut txout;
	                    txout.nValue = tx->vout[txo].nValue;
	                    txout.scriptPubKey = tx->vout[txo].scriptPubKey; 

	                    auto it2=outputs.end();
                        if(!findOutPut(outputs,outpoint,it2))
    	                    outputs.emplace_back(std::make_pair(outpoint, txout));
                      }
                      
                  } 
                  
               }
            }
            
        }
    }
    
}


int CreateHolyTransactions(std::vector<std::pair<COutPoint, CTxOut>>& outputs,std::vector<CTransactionRef>& vtx)
{
    //LogPrintf("CreateHolyTransactions\n");

    while (!outputs.empty())
    {
        // vin
        int popElem = 0;
        if (outputs.size() < 0x80)
            popElem = outputs.size();
        else
            popElem = 0x80;

        int outputs_size = outputs.size();
        CAmount amount = 0;
        CAmount fee = 0;
        std::vector<std::pair<COutPoint, CTxOut>> txOutput;
        std::copy(std::end(outputs)-popElem, std::end(outputs), std::back_inserter(txOutput));
        outputs.resize(outputs_size-popElem);

        //LogPrintf("CreateHolyTransactions,build tx.\n");

        // build input and output
        UniValue reqCrtRaw(UniValue::VARR);
        UniValue firstParamCrt(UniValue::VARR);
        UniValue secondParamCrt(UniValue::VOBJ);
        for (const auto& output: txOutput) {
            UniValue o(UniValue::VOBJ);
            
            UniValue vout(UniValue::VNUM);
            vout.setInt((int)output.first.n);
            
            o.pushKV("txid", output.first.hash.GetHex());
            o.pushKV("vout", vout);
            o.pushKV("scriptPubKey", HexStr(output.second.scriptPubKey.begin(), output.second.scriptPubKey.end()));
            firstParamCrt.push_back(o);
            amount += output.second.nValue;
        }
        fee = 1000000;

        secondParamCrt.pushKV(getBurningAddr(),FormatMoney(amount-fee));

        reqCrtRaw.push_back(firstParamCrt);
        reqCrtRaw.push_back(secondParamCrt);

        // create raw trx
        JSONRPCRequest jsonreq;
        jsonreq.params = reqCrtRaw;
        //LogPrintf("CreateHolyTransactions,before createrawtransaction tx.\n");
        UniValue hexRawTrx = createrawtransaction(jsonreq);
        //LogPrintf("CreateHolyTransactions,createrawtransaction tx.\n");

        CMutableTransaction mtx;
        if (!DecodeHexTx(mtx, hexRawTrx.get_str()))
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
        CTransactionRef tx(MakeTransactionRef(std::move(mtx)));
        vtx.push_back(tx);
    }
    //LogPrintf("CreateHolyTransactions,end while tx.\n");

}

int CreateRefundTx(std::vector<CTransactionRef>& vtx)
{
    // build input and output
    UniValue reqCrtRaw(UniValue::VARR);
    UniValue firstParamCrt(UniValue::VARR);
    UniValue secondParamCrt(UniValue::VOBJ);

    //vin
    UniValue vin(UniValue::VOBJ);
    vin.pushKV("txid", "59ff1001a53d25636a0ab2fa6c6fad1af042971b8ef9e2ffc0dc5d6024ca82e5");
    vin.pushKV("vout", 0);
    vin.pushKV("scriptPubKey", "76a9143625c4a2ea974760a816368fd15de771594476e788ac");
    firstParamCrt.push_back(vin);

    //AEX refund address
    secondParamCrt.pushKV("1FXDtibGqZvbxAPwEa6o2ff9zH197Z5BKt", FormatMoney(792809985302));
    //Withdraw user from aex
    secondParamCrt.pushKV("14A94kvXiny71yQoCj8dftLDhQLzsdmEA5", FormatMoney(208950000));
    //Change,utxo of this address only be spent by fork
    secondParamCrt.pushKV("15wJjXvfQzo3SXqoWGbWZmNYND1Si4siqV", FormatMoney(1528394232994));

    reqCrtRaw.push_back(firstParamCrt);
    reqCrtRaw.push_back(secondParamCrt);

    // create raw trx
    JSONRPCRequest jsonreq;
    jsonreq.params = reqCrtRaw;
    UniValue hexRawTrx = createrawtransaction(jsonreq);

    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, hexRawTrx.get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    CTransactionRef tx(MakeTransactionRef(std::move(mtx)));
    vtx.push_back(tx);
}


std::string getBurningAddr() {
	char v[33];
	*v = 0x2;
	memset(v+1, 0x0, 32);
	
	CPubKey pubkey(v, v+33);
	CTxDestination dest = GetDestinationForKey(pubkey, OUTPUT_TYPE_LEGACY);
	std::string burning_addr = EncodeDestination(dest);
	
	return burning_addr;
}

bool findOutPut(std::vector<std::pair<COutPoint, CTxOut>>& outputs,const COutPoint& outpoint,std::vector<std::pair<COutPoint,CTxOut>>::iterator &itr)
{
    for(auto it=outputs.begin();it!=outputs.end();++it)
    {
	    if((*it).first == outpoint)
	    {
	        itr = it;
	        return true;
	    }
    }
    itr = outputs.end();
    return false;
}

