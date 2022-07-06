// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/miner.h>

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <pos.h>
#include <primitives/transaction.h>
#include <timedata.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <qtum/qtumledger.h>
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#endif

#include <algorithm>
#include <utility>

namespace node {
unsigned int nMaxStakeLookahead = MAX_STAKE_LOOKAHEAD;
unsigned int nBytecodeTimeBuffer = BYTECODE_TIME_BUFFER;
unsigned int nStakeTimeBuffer = STAKE_TIME_BUFFER;
unsigned int nMinerSleep = STAKER_POLLING_PERIOD;
unsigned int nMinerWaitWalidBlock = STAKER_WAIT_FOR_WALID_BLOCK;
unsigned int nMinerWaitBestBlockHeader = STAKER_WAIT_FOR_BEST_BLOCK_HEADER;

void updateMinerParams(int nHeight, const Consensus::Params& consensusParams, bool minDifficulty)
{
    static unsigned int timeDownscale = 1;
    static unsigned int timeDefault = 1;
    unsigned int timeDownscaleTmp = consensusParams.TimestampDownscaleFactor(nHeight);
    if(timeDownscale != timeDownscaleTmp)
    {
        timeDownscale = timeDownscaleTmp;
        unsigned int targetSpacing =  consensusParams.TargetSpacing(nHeight);
        nMaxStakeLookahead = std::max(MAX_STAKE_LOOKAHEAD / timeDownscale, timeDefault);
        nMaxStakeLookahead = std::min(nMaxStakeLookahead, targetSpacing);
        nBytecodeTimeBuffer = std::max(BYTECODE_TIME_BUFFER / timeDownscale, timeDefault);
        nStakeTimeBuffer = std::max(STAKE_TIME_BUFFER / timeDownscale, timeDefault);
        nMinerSleep = std::max(STAKER_POLLING_PERIOD / timeDownscale, timeDefault);
        nMinerWaitWalidBlock = std::max(STAKER_WAIT_FOR_WALID_BLOCK / timeDownscale, timeDefault);
    }

    // Sleep for 20 seconds when mining with minimum difficulty to avoid creating blocks every 4 seconds
    if(minDifficulty && nMinerSleep != STAKER_POLLING_PERIOD_MIN_DIFFICULTY)
    {
        nMinerSleep = STAKER_POLLING_PERIOD_MIN_DIFFICULTY;
    }
}

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast() + 1, GetAdjustedTime());

    if (nOldTime < nNewTime) {
        pblock->nTime = nNewTime;
    }

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams, pblock->IsProofOfStake());
    }

    return nNewTime - nOldTime;
}

void RegenerateCommitments(CBlock& block, ChainstateManager& chainman)
{
    CMutableTransaction tx{*block.vtx.at(0)};
    tx.vout.erase(tx.vout.begin() + GetWitnessCommitmentIndex(block));
    block.vtx.at(0) = MakeTransactionRef(tx);

    CBlockIndex* prev_block = WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock));
    GenerateCoinbaseCommitment(block, prev_block, Params().GetConsensus());

    block.hashMerkleRoot = BlockMerkleRoot(block);
}

BlockAssembler::Options::Options()
{
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
}

BlockAssembler::BlockAssembler(CChainState& chainstate, const CTxMemPool& mempool, const CChainParams& params, const Options& options)
    : chainparams(params),
      m_mempool(mempool),
      m_chainstate(chainstate)
{
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit weight to between 4K and dgpMaxBlockWeight-4K for sanity:
    nBlockMaxWeight = std::max<size_t>(4000, std::min<size_t>(dgpMaxBlockWeight - 4000, options.nBlockMaxWeight));
}

static BlockAssembler::Options DefaultOptions()
{
    // Block resource limits
    // If -blockmaxweight is not given, limit to DEFAULT_BLOCK_MAX_WEIGHT
    BlockAssembler::Options options;
    options.nBlockMaxWeight = gArgs.GetIntArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
    if (gArgs.IsArgSet("-blockmintxfee")) {
        std::optional<CAmount> parsed = ParseMoney(gArgs.GetArg("-blockmintxfee", ""));
        options.blockMinFeeRate = CFeeRate{parsed.value_or(DEFAULT_BLOCK_MIN_TX_FEE)};
    } else {
        options.blockMinFeeRate = CFeeRate{DEFAULT_BLOCK_MIN_TX_FEE};
    }
    return options;
}

BlockAssembler::BlockAssembler(CChainState& chainstate, const CTxMemPool& mempool, const CChainParams& params)
    : BlockAssembler(chainstate, mempool, params, DefaultOptions()) {}

#ifdef ENABLE_WALLET
BlockAssembler::BlockAssembler(CChainState& chainstate, const CTxMemPool& mempool, const CChainParams& params, wallet::CWallet *_pwallet)
    : BlockAssembler(chainstate, mempool, params)
{
    pwallet = _pwallet;
}
#endif

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;
    fIncludeWitness = false;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

void BlockAssembler::RebuildRefundTransaction(CBlock* pblock){
    int refundtx=0; //0 for coinbase in PoW
    if(pblock->IsProofOfStake()){
        refundtx=1; //1 for coinstake in PoS
    }
    CMutableTransaction contrTx(originalRewardTx);
    contrTx.vout[refundtx].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    contrTx.vout[refundtx].nValue -= bceResult.refundSender;
    //note, this will need changed for MPoS
    int i=contrTx.vout.size();
    contrTx.vout.resize(contrTx.vout.size()+bceResult.refundOutputs.size());
    for(CTxOut& vout : bceResult.refundOutputs){
        contrTx.vout[i]=vout;
        i++;
    }
    pblock->vtx[refundtx] = MakeTransactionRef(std::move(contrTx));
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, bool fMineWitnessTx, bool fProofOfStake, int64_t* pTotalFees, int32_t txProofTime, int32_t nTimeLimit)
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if (!pblocktemplate.get()) {
        return nullptr;
    }
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    this->nTimeLimit = nTimeLimit;

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    // Add dummy coinstake tx as second transaction
    if(fProofOfStake)
        pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

#ifdef ENABLE_WALLET
    if(pwallet && pwallet->IsStakeClosing())
        return nullptr;
#endif
    LOCK2(cs_main, m_mempool.cs);
    CBlockIndex* pindexPrev = m_chainstate.m_chain.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = g_versionbitscache.ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand()) {
        pblock->nVersion = gArgs.GetIntArg("-blockversion", pblock->nVersion);
    }

    if(txProofTime == 0) {
        txProofTime = GetAdjustedTime();
    }
    if(fProofOfStake)
        txProofTime &= ~chainparams.GetConsensus().StakeTimestampMask(nHeight);
    pblock->nTime = txProofTime;
    if (!fProofOfStake)
        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus(),fProofOfStake);
    m_lock_time_cutoff = pindexPrev->GetMedianTimePast();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization).
    // Note that the mempool would accept transactions with witness data before
    // the deployment is active, but we would only ever mine blocks after activation
    // unless there is a massive block reorganization with the witness softfork
    // not activated.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = DeploymentActiveAfter(pindexPrev, chainparams.GetConsensus(), Consensus::DEPLOYMENT_SEGWIT);

    int64_t nTime1 = GetTimeMicros();

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
    if (fProofOfStake)
    {
        // Make the coinbase tx empty in case of proof of stake
        coinbaseTx.vout[0].SetEmpty();
    }
    else
    {
        coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
        coinbaseTx.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    }
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    originalRewardTx = coinbaseTx;
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));

    // Create coinstake transaction.
    if(fProofOfStake)
    {
        CMutableTransaction coinstakeTx;
        coinstakeTx.vout.resize(2);
        coinstakeTx.vout[0].SetEmpty();
        coinstakeTx.vout[1].scriptPubKey = scriptPubKeyIn;
        originalRewardTx = coinstakeTx;
        pblock->vtx[1] = MakeTransactionRef(std::move(coinstakeTx));

        //this just makes CBlock::IsProofOfStake to return true
        //real prevoutstake info is filled in later in SignBlock
        pblock->prevoutStake.n=0;

    }

    //////////////////////////////////////////////////////// qtum
    QtumDGP qtumDGP(globalState.get(), m_chainstate, fGettingValuesDGP);
    globalSealEngine->setQtumSchedule(qtumDGP.getGasSchedule(nHeight));
    uint32_t blockSizeDGP = qtumDGP.getBlockSize(nHeight);
    minGasPrice = qtumDGP.getMinGasPrice(nHeight);
    if(gArgs.IsArgSet("-staker-min-tx-gas-price")) {
        std::optional<CAmount> stakerMinGasPrice = ParseMoney(gArgs.GetArg("-staker-min-tx-gas-price", ""));
        minGasPrice = std::max(minGasPrice, (uint64_t)(stakerMinGasPrice.value_or(0)));
    }
    hardBlockGasLimit = qtumDGP.getBlockGasLimit(nHeight);
    softBlockGasLimit = gArgs.GetIntArg("-staker-soft-block-gas-limit", hardBlockGasLimit);
    softBlockGasLimit = std::min(softBlockGasLimit, hardBlockGasLimit);
    txGasLimit = gArgs.GetIntArg("-staker-max-tx-gas-limit", softBlockGasLimit);

    nBlockMaxWeight = blockSizeDGP ? blockSizeDGP * WITNESS_SCALE_FACTOR : nBlockMaxWeight;
    
    dev::h256 oldHashStateRoot(globalState->rootHash());
    dev::h256 oldHashUTXORoot(globalState->rootHashUTXO());
    ////////////////////////////////////////////////// deploy offline staking contract
    if(nHeight == chainparams.GetConsensus().nOfflineStakeHeight){
        globalState->deployDelegationsContract();
    }
    /////////////////////////////////////////////////
    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated, minGasPrice, pblock);
    pblock->hashStateRoot = uint256(h256Touint(dev::h256(globalState->rootHash())));
    pblock->hashUTXORoot = uint256(h256Touint(dev::h256(globalState->rootHashUTXO())));
    globalState->setRoot(oldHashStateRoot);
    globalState->setRootUTXO(oldHashUTXORoot);

    //this should already be populated by AddBlock in case of contracts, but if no contracts
    //then it won't get populated
    RebuildRefundTransaction(pblock);
    ////////////////////////////////////////////////////////

    pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus(), fProofOfStake);
    pblocktemplate->vTxFees[0] = -nFees;

    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    // The total fee is the Fees minus the Refund
    if (pTotalFees)
        *pTotalFees = nFees - bceResult.refundSender;

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    pblock->nNonce         = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    BlockValidationState state;
    if (!fProofOfStake && !TestBlockValidity(state, chainparams, m_chainstate, *pblock, pindexPrev, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, state.ToString()));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n", 0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1), 0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateEmptyBlock(const CScript& scriptPubKeyIn, bool fMineWitnessTx, bool fProofOfStake, int64_t* pTotalFees, int32_t nTime)
{
    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return nullptr;
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    // Add dummy coinstake tx as second transaction
    if(fProofOfStake)
        pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

#ifdef ENABLE_WALLET
    if(pwallet && pwallet->IsStakeClosing())
        return nullptr;
#endif
    LOCK2(cs_main, m_mempool.cs);
    CBlockIndex* pindexPrev = m_chainstate.m_chain.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = g_versionbitscache.ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = gArgs.GetIntArg("-blockversion", pblock->nVersion);

    uint32_t txProofTime = nTime == 0 ? GetAdjustedTime() : nTime;
    if(fProofOfStake)
        txProofTime &= ~chainparams.GetConsensus().StakeTimestampMask(nHeight);
    pblock->nTime = txProofTime;

    m_lock_time_cutoff = pindexPrev->GetMedianTimePast();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization) or when
    // -promiscuousmempoolflags is used.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = DeploymentActiveAfter(pindexPrev, chainparams.GetConsensus(), Consensus::DEPLOYMENT_SEGWIT) && fMineWitnessTx;

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
    if (fProofOfStake)
    {
        // Make the coinbase tx empty in case of proof of stake
        coinbaseTx.vout[0].SetEmpty();
    }
    else
    {
        coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
        coinbaseTx.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    }
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    originalRewardTx = coinbaseTx;
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));

    // Create coinstake transaction.
    if(fProofOfStake)
    {
        CMutableTransaction coinstakeTx;
        coinstakeTx.vout.resize(2);
        coinstakeTx.vout[0].SetEmpty();
        coinstakeTx.vout[1].scriptPubKey = scriptPubKeyIn;
        originalRewardTx = coinstakeTx;
        pblock->vtx[1] = MakeTransactionRef(std::move(coinstakeTx));

        //this just makes CBlock::IsProofOfStake to return true
        //real prevoutstake info is filled in later in SignBlock
        pblock->prevoutStake.n=0;
    }

    //////////////////////////////////////////////////////// qtum
    //state shouldn't change here for an empty block, but if it's not valid it'll fail in CheckBlock later
    pblock->hashStateRoot = uint256(h256Touint(dev::h256(globalState->rootHash())));
    pblock->hashUTXORoot = uint256(h256Touint(dev::h256(globalState->rootHashUTXO())));

    RebuildRefundTransaction(pblock);
    ////////////////////////////////////////////////////////

    pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus(), fProofOfStake);
    pblocktemplate->vTxFees[0] = -nFees;

    // The total fee is the Fees minus the Refund
    if (pTotalFees)
        *pTotalFees = nFees - bceResult.refundSender;

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    if (!fProofOfStake)
        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus(),fProofOfStake);
    pblock->nNonce         = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    BlockValidationState state;
    if (!fProofOfStake && !TestBlockValidity(state, chainparams, m_chainstate, *pblock, pindexPrev, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, state.ToString()));
    }

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        } else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight) {
        return false;
    }
    if (nBlockSigOpsCost + packageSigOpsCost >= (uint64_t)dgpMaxBlockSigOps) {
        return false;
    }
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package) const
{
    for (CTxMemPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, m_lock_time_cutoff)) {
            return false;
        }
        if (!fIncludeWitness && it->GetTx().HasWitness()) {
            return false;
        }
    }
    return true;
}

bool BlockAssembler::AttemptToAddContractToBlock(CTxMemPool::txiter iter, uint64_t minGasPrice, CBlock* pblock) {
    if (nTimeLimit != 0 && GetAdjustedTime() >= nTimeLimit - nBytecodeTimeBuffer) {
        return false;
    }
    if (gArgs.GetBoolArg("-disablecontractstaking", false))
    {
        // Contract staking is disabled for the staker
        return false;
    }
    
    dev::h256 oldHashStateRoot(globalState->rootHash());
    dev::h256 oldHashUTXORoot(globalState->rootHashUTXO());
    // operate on local vars first, then later apply to `this`
    uint64_t nBlockWeight = this->nBlockWeight;
    uint64_t nBlockSigOpsCost = this->nBlockSigOpsCost;

    unsigned int contractflags = GetContractScriptFlags(nHeight, chainparams.GetConsensus());
    QtumTxConverter convert(iter->GetTx(), m_chainstate, &m_mempool, NULL, &pblock->vtx, contractflags);

    ExtractQtumTX resultConverter;
    if(!convert.extractionQtumTransactions(resultConverter)){
        //this check already happens when accepting txs into mempool
        //therefore, this can only be triggered by using raw transactions on the staker itself
        LogPrintf("AttemptToAddContractToBlock(): Fail to extract contacts from tx %s\n", iter->GetTx().GetHash().ToString());
        return false;
    }
    std::vector<QtumTransaction> qtumTransactions = resultConverter.first;
    dev::u256 txGas = 0;
    for(QtumTransaction qtumTransaction : qtumTransactions){
        txGas += qtumTransaction.gas();
        if(txGas > txGasLimit) {
            // Limit the tx gas limit by the soft limit if such a limit has been specified.
            LogPrintf("AttemptToAddContractToBlock(): The gas needed is bigger than -staker-max-tx-gas-limit for the contract tx %s\n", iter->GetTx().GetHash().ToString());
            return false;
        }

        if(bceResult.usedGas + qtumTransaction.gas() > softBlockGasLimit){
            // If this transaction's gasLimit could cause block gas limit to be exceeded, then don't add it
            // Log if the contract is the only contract tx
            if(bceResult.usedGas == 0)
                LogPrintf("AttemptToAddContractToBlock(): The gas needed is bigger than -staker-soft-block-gas-limit for the contract tx %s\n", iter->GetTx().GetHash().ToString());
            return false;
        }
        if(qtumTransaction.gasPrice() < minGasPrice){
            //if this transaction's gasPrice is less than the current DGP minGasPrice don't add it
            LogPrintf("AttemptToAddContractToBlock(): The gas price is less than -staker-min-tx-gas-price for the contract tx %s\n", iter->GetTx().GetHash().ToString());
            return false;
        }
    }
    // We need to pass the DGP's block gas limit (not the soft limit) since it is consensus critical.
    ByteCodeExec exec(*pblock, qtumTransactions, hardBlockGasLimit, m_chainstate.m_chain.Tip(), m_chainstate.m_chain);
    if(!exec.performByteCode()){
        //error, don't add contract
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
        LogPrintf("AttemptToAddContractToBlock(): Perform byte code fails for the contract tx %s\n", iter->GetTx().GetHash().ToString());
        return false;
    }

    ByteCodeExecResult testExecResult;
    if(!exec.processingResults(testExecResult)){
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
        LogPrintf("AttemptToAddContractToBlock(): Processing results fails for the contract tx %s\n", iter->GetTx().GetHash().ToString());
        return false;
    }

    if(bceResult.usedGas + testExecResult.usedGas > softBlockGasLimit){
        // If this transaction could cause block gas limit to be exceeded, then don't add it
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
        // Log if the contract is the only contract tx
        if(bceResult.usedGas == 0)
            LogPrintf("AttemptToAddContractToBlock(): The gas used is bigger than -staker-soft-block-gas-limit for the contract tx %s\n", iter->GetTx().GetHash().ToString());
        return false;
    }

    //apply contractTx costs to local state
    nBlockWeight += iter->GetTxWeight();
    nBlockSigOpsCost += iter->GetSigOpCost();
    //apply value-transfer txs to local state
    for (CTransaction &t : testExecResult.valueTransfers) {
        nBlockWeight += GetTransactionWeight(t);
        nBlockSigOpsCost += GetLegacySigOpCount(t);
    }

    int proofTx = pblock->IsProofOfStake() ? 1 : 0;

    //calculate sigops from new refund/proof tx

    //first, subtract old proof tx
    nBlockSigOpsCost -= GetLegacySigOpCount(*pblock->vtx[proofTx]);

    // manually rebuild refundtx
    CMutableTransaction contrTx(*pblock->vtx[proofTx]);
    //note, this will need changed for MPoS
    int i=contrTx.vout.size();
    contrTx.vout.resize(contrTx.vout.size()+testExecResult.refundOutputs.size());
    for(CTxOut& vout : testExecResult.refundOutputs){
        contrTx.vout[i]=vout;
        i++;
    }
    nBlockSigOpsCost += GetLegacySigOpCount(contrTx);
    //all contract costs now applied to local state

    //Check if block will be too big or too expensive with this contract execution
    if (nBlockSigOpsCost * WITNESS_SCALE_FACTOR > (uint64_t)dgpMaxBlockSigOps ||
            nBlockWeight > dgpMaxBlockWeight) {
        //contract will not be added to block, so revert state to before we tried
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
        return false;
    }

    //block is not too big, so apply the contract execution and it's results to the actual block

    //apply local bytecode to global bytecode state
    bceResult.usedGas += testExecResult.usedGas;
    bceResult.refundSender += testExecResult.refundSender;
    bceResult.refundOutputs.insert(bceResult.refundOutputs.end(), testExecResult.refundOutputs.begin(), testExecResult.refundOutputs.end());
    bceResult.valueTransfers = std::move(testExecResult.valueTransfers);

    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    this->nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    this->nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    for (CTransaction &t : bceResult.valueTransfers) {
        pblock->vtx.emplace_back(MakeTransactionRef(std::move(t)));
        this->nBlockWeight += GetTransactionWeight(t);
        this->nBlockSigOpsCost += GetLegacySigOpCount(t);
        ++nBlockTx;
    }
    //calculate sigops from new refund/proof tx
    this->nBlockSigOpsCost -= GetLegacySigOpCount(*pblock->vtx[proofTx]);
    RebuildRefundTransaction(pblock);
    this->nBlockSigOpsCost += GetLegacySigOpCount(*pblock->vtx[proofTx]);

    bceResult.valueTransfers.clear();

    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblocktemplate->block.vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee rate %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries& alreadyAdded,
        indexed_modified_transaction_set &mapModifiedTx)
{
    AssertLockHeld(m_mempool.cs);

    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        m_mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc)) {
                continue;
            }
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
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set& mapModifiedTx, CTxMemPool::setEntries& failedTx)
{
    AssertLockHeld(m_mempool.cs);

    assert(it != m_mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
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
void BlockAssembler::addPackageTxs(int& nPackagesSelected, int& nDescendantsUpdated, uint64_t minGasPrice, CBlock* pblock)
{
    AssertLockHeld(m_mempool.cs);

    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score_or_gas_price>::type::iterator mi = m_mempool.mapTx.get<ancestor_score_or_gas_price>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != m_mempool.mapTx.get<ancestor_score_or_gas_price>().end() || !mapModifiedTx.empty()) {
        if(nTimeLimit != 0 && GetAdjustedTime() >= nTimeLimit){
            //no more time to add transactions, just exit
            return;
        }
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != m_mempool.mapTx.get<ancestor_score_or_gas_price>().end() &&
            SkipMapTxEntry(m_mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score_or_gas_price>().begin();
        if (mi == m_mempool.mapTx.get<ancestor_score_or_gas_price>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = m_mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score_or_gas_price>().end() &&
                    CompareModifiedEntry()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
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

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                    nBlockMaxWeight - 4000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        m_mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

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
        SortForBlock(ancestors, sortedEntries);

        bool wasAdded=true;
        for (size_t i=0; i<sortedEntries.size(); ++i) {
            if(!wasAdded || (nTimeLimit != 0 && GetAdjustedTime() >= nTimeLimit))
            {
                //if out of time, or earlier ancestor failed, then skip the rest of the transactions
                mapModifiedTx.erase(sortedEntries[i]);
                wasAdded=false;
                continue;
            }
            const CTransaction& tx = sortedEntries[i]->GetTx();
            if(wasAdded) {
                if (tx.HasCreateOrCall()) {
                    wasAdded = AttemptToAddContractToBlock(sortedEntries[i], minGasPrice, pblock);
                    if(!wasAdded){
                        if(fUsingModified) {
                            //this only needs to be done once to mark the whole package (everything in sortedEntries) as failed
                            mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                            failedTx.insert(iter);
                        }
                    }
                } else {
                    AddToBlock(sortedEntries[i]);
                }
            }
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        if(!wasAdded){
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
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight + 1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce));
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

bool CanStake()
{
    bool canStake = gArgs.GetBoolArg("-staking", DEFAULT_STAKE);

    if(canStake)
    {
        // Signet is for creating PoW blocks by an authorized signer
        canStake = !Params().GetConsensus().signet_blocks;
    }

    return canStake;
}

#ifdef ENABLE_WALLET
//////////////////////////////////////////////////////////////////////////////
//
// Proof of Stake miner
//

//
// Looking for suitable coins for creating new block.
//

bool SignBlockHWI(std::shared_ptr<CBlock> pblock, wallet::CWallet& wallet, std::vector<unsigned char>& vchSig)
{
    // Check ledger ID
    if(wallet.m_ledger_id == "") {
        return false;
    }
    QtumLedger &device = QtumLedger::instance();

    // Make a blank psbt
    PartiallySignedTransaction psbtx_in;
    CMutableTransaction rawTx = CMutableTransaction(*pblock->vtx[1]);
    psbtx_in.tx = rawTx;
    for (unsigned int i = 0; i < rawTx.vin.size(); ++i) {
        psbtx_in.inputs.push_back(PSBTInput());
    }
    for (unsigned int i = 0; i < rawTx.vout.size(); ++i) {
        psbtx_in.outputs.push_back(PSBTOutput());
    }

    // Get staker path
    CScript stakerPubKey = rawTx.vout[1].scriptPubKey;
    CTxDestination txStakerDest = ExtractPublicKeyHash(stakerPubKey);
    std::string strStaker;
    if(!wallet.GetHDKeyPath(txStakerDest, strStaker)) {
        return false;
    }

    // Fill transaction with out data but don't sign
    bool bip32derivs = true;
    bool complete = true;
    const TransactionError err = wallet.FillPSBT(psbtx_in, complete, 1, false, bip32derivs);
    if (err != TransactionError::OK) {
        return false;
    }

    // Serialize the PSBT
    if(wallet.IsStakeClosing()) return false;
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx_in;
    std::string psbt = EncodeBase64(ssTx.str());
    if(!device.signCoinStake(wallet.m_ledger_id, psbt)) {
        return false;
    }

    // Unserialize the transactions
    PartiallySignedTransaction psbtx_out;
    std::string error;
    if (!DecodeBase64PSBT(psbtx_out, psbt, error)) {
        return false;
    }

    // Update block proof
    CMutableTransaction txCoinStake;
    complete = FinalizeAndExtractPSBT(psbtx_out, txCoinStake);
    if(!complete) {
        return false;
    }
    pblock->vtx[1] = MakeTransactionRef(std::move(txCoinStake));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);

    // Sign block header
    if(wallet.IsStakeClosing()) return false;
    std::string header = pblock->GetWithoutSign();
    if(!device.signBlockHeader(wallet.m_ledger_id, header, strStaker, vchSig)) {
        return false;
    }

    return true;
}

bool SignBlockLedger(std::shared_ptr<CBlock> pblock, wallet::CWallet& wallet)
{
    LOCK(cs_ledger);
    std::vector<unsigned char> vchSig;
    bool ret = SignBlockHWI(pblock, wallet, vchSig);
    if(ret) pblock->SetBlockSignature(vchSig);
    if(!ret && !wallet.IsStakeClosing())
    {
        std::string errorMessage = QtumLedger::instance().errorMessage();
        LogPrintf("WARN: %s: fail to sign block (%s)\n", __func__, errorMessage);
    }
    return ret;
}

// novacoin: attempt to generate suitable proof-of-stake
bool SignBlock(std::shared_ptr<CBlock> pblock, wallet::CWallet& wallet, const CAmount& nTotalFees, uint32_t nTime, std::set<std::pair<const wallet::CWalletTx*,unsigned int> >& setCoins, std::vector<COutPoint>& setSelectedCoins, std::vector<COutPoint>& setDelegateCoins, bool selectedOnly = false, bool tryOnly = false)
{
    // if we are trying to sign
    //    something except proof-of-stake block template
    if (!CheckFirstCoinstakeOutput(*pblock))
        return false;

    // if we are trying to sign
    //    a complete proof-of-stake block
    if (pblock->IsProofOfStake() && !pblock->vchBlockSigDlgt.empty())
        return true;

    PKHash pkhash;
    CMutableTransaction txCoinStake(*pblock->vtx[1]);
    uint32_t nTimeBlock = nTime;
    std::vector<unsigned char> vchPoD;
    COutPoint headerPrevout;
    //original line:
    //int64_t nSearchInterval = IsProtocolV2(nBestHeight+1) ? 1 : nSearchTime - nLastCoinStakeSearchTime;
    //IsProtocolV2 mean POS 2 or higher, so the modified line is:
    if(wallet.IsStakeClosing()) return false;
    LOCK(wallet.cs_wallet);
    uint32_t nHeight = wallet.chain().getHeight().value_or(0) + 1;
    const Consensus::Params& consensusParams = Params().GetConsensus();
    nTimeBlock &= ~consensusParams.StakeTimestampMask(nHeight);
    bool privateKeysDisabled = wallet.IsWalletFlagSet(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    bool found = false;
    {
        LOCK(cs_main);
        found = wallet.CreateCoinStake(pblock->nBits, nTotalFees, nTimeBlock, txCoinStake, pkhash, setCoins, setSelectedCoins, setDelegateCoins, selectedOnly, !privateKeysDisabled, vchPoD, headerPrevout);
    }
    if (found)
    {
        if (nTimeBlock >= wallet.chain().getTip()->GetMedianTimePast()+1)
        {
            // make sure coinstake would meet timestamp protocol
            //    as it would be the same as the block timestamp
            pblock->nTime = nTimeBlock;
            pblock->vtx[1] = MakeTransactionRef(std::move(txCoinStake));
            pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
            pblock->prevoutStake = headerPrevout;

            if(tryOnly)
                return true;

            // Check timestamp against prev
            if(pblock->GetBlockTime() <= wallet.chain().getTip()->GetBlockTime() || FutureDrift(pblock->GetBlockTime(), nHeight, consensusParams) < wallet.chain().getTip()->GetBlockTime())
            {
                return false;
            }

            // Sign block
            if (wallet.chain().getHeight().value_or(0) + 1 >= consensusParams.nOfflineStakeHeight)
            {
                // append PoD to the end of the block header
                if(vchPoD.size() > 0)
                    pblock->SetProofOfDelegation(vchPoD);

                // append a signature to our block, ensure that is compact and check block header
                bool isSigned = privateKeysDisabled ? SignBlockLedger(pblock, wallet) : wallet.SignBlockStake(*pblock, pkhash, true);
                return isSigned && CheckHeaderProof(*pblock, consensusParams, wallet.chain().chainman().ActiveChainstate());
            }
            else
            {
                // append a signature to our block and ensure that is LowS
                return wallet.SignBlockStake(*pblock, pkhash, false) &&
                           EnsureLowS(pblock->vchBlockSigDlgt) &&
                           CheckHeaderProof(*pblock, consensusParams, wallet.chain().chainman().ActiveChainstate());
            }
        }
    }

    return false;
}
#endif
} // namespace node
