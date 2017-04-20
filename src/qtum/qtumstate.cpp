#include <sstream>
#include <util.h>
#include "qtumstate.h"

using namespace std;
using namespace dev;
using namespace dev::eth;

QtumState::QtumState(u256 const& _accountStartNonce, OverlayDB const& _db, const string& _path, BaseState _bs) :
        State(_accountStartNonce, _db, _bs) {
            dbUTXO = QtumState::openDB(_path + "/qtumDB", sha3(rlp("")), WithExisting::Trust);
	        stateUTXO = SecureTrieDB<Address, OverlayDB>(&dbUTXO);
}

QtumState::QtumState() : dev::eth::State(dev::Invalid256, dev::OverlayDB(), dev::eth::BaseState::PreExisting) {
    dbUTXO = OverlayDB();
    stateUTXO = SecureTrieDB<Address, OverlayDB>(&dbUTXO);
}

pair<ExecutionResult, TransactionReceipt> QtumState::execute(EnvInfo const& _envInfo, SealEngineFace const& _sealEngine, QtumTransaction const& _t, Permanence _p, OnOpFunc const& _onOp){
    addBalance(_t.sender(), _t.value() + (_t.gas() * _t.gasPrice()));
    newAddress = _t.isCreation() ? createQtumAddress(_t.getHashWith(), _t.getNVout()) : dev::Address();

	auto onOp = _onOp;
#if ETH_VMTRACE
	if (isChannelVisible<VMTraceChannel>())
		onOp = Executive::simpleTrace(); // override tracer
#endif

	// Create and initialize the executive. This will throw fairly cheaply and quickly if the
	// transaction is bad in any way.
	Executive e(*this, _envInfo, _sealEngine);
	ExecutionResult res;
	e.setResultRecipient(res);

    u256 startGasUsed;
    try{
        e.initialize(_t);
        // OK - transaction looks valid - execute.
        startGasUsed = _envInfo.gasUsed();
        if (!e.execute())
            e.go(onOp);
        e.finalize();

        if (_p == Permanence::Reverted){
            m_cache.clear();
            cacheUTXO.clear();
        } else {
            std::vector<Address> deleteAddresses = {_t.sender(), _envInfo.author()};
            deleteAccounts(deleteAddresses);
            CondensingTX ctx(this, transfers, _t);
            CTransaction tx = ctx.createCondensingTX();
            std::unordered_map<dev::Address, Vin> vins = ctx.createVin(tx);
            updateUTXO(vins);
            
            qtum::commit(cacheUTXO, stateUTXO, m_cache);
            cacheUTXO.clear();

            bool removeEmptyAccounts = _envInfo.number() >= _sealEngine.chainParams().u256Param("EIP158ForkBlock");
            commit(removeEmptyAccounts ? State::CommitBehaviour::RemoveEmptyAccounts : State::CommitBehaviour::KeepEmptyAccounts);
        }
    }
    catch(Exception const& _e){
        std::stringstream exception;
        exception << dev::eth::toTransactionException(_e);
        LogPrintf("VMException: %s\n", exception.str());
        res.excepted = dev::eth::toTransactionException(_e);

        if(_p != Permanence::Reverted){
            std::vector<Address> deleteAddresses = {_t.sender()};
            deleteAccounts(deleteAddresses);
            commit(CommitBehaviour::RemoveEmptyAccounts);
        }
    }

    if(!_t.isCreation())
        res.newAddress = _t.receiveAddress();
    newAddress = dev::Address();
    transfers.clear();
	return make_pair(res, dev::eth::TransactionReceipt(rootHash(), startGasUsed + e.gasUsed(), e.logs()));
}

std::unordered_map<dev::Address, Vin> QtumState::vins() const // temp
{
    std::unordered_map<dev::Address, Vin> ret;
    for (auto& i: cacheUTXO)
        if (i.second.alive)
            ret[i.first] = i.second;
    auto addrs = addresses();
    for (auto& i : addrs){
        if (cacheUTXO.find(i.first) == cacheUTXO.end() && vin(i.first))
            ret[i.first] = *vin(i.first);
    }
    return ret;
}

void QtumState::transferBalance(dev::Address const& _from, dev::Address const& _to, dev::u256 const& _value) {
    subBalance(_from, _value);
    addBalance(_to, _value);
    transfers.push_back({_from, _to, _value});
}

Vin const* QtumState::vin(dev::Address const& _a) const
{
    return const_cast<QtumState*>(this)->vin(_a);
}

Vin* QtumState::vin(dev::Address const& _addr)
{
    auto it = cacheUTXO.find(_addr);
    if (it == cacheUTXO.end()){
        std::string stateBack = stateUTXO.at(_addr);
        if (stateBack.empty())
            return nullptr;
            
        dev::RLP state(stateBack);
        auto i = cacheUTXO.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(_addr),
            std::forward_as_tuple(Vin{state[0].toHash<dev::h256>(), state[1].toInt<uint32_t>(), state[2].toInt<dev::u256>(), state[3].toInt<uint8_t>()})
        );
        return &i.first->second;
    }
    return &it->second;
}

// void QtumState::commit(CommitBehaviour _commitBehaviour)
// {
//     if (_commitBehaviour == CommitBehaviour::RemoveEmptyAccounts)
//         removeEmptyAccounts();

//     qtum::commit(cacheUTXO, stateUTXO, m_cache);
//     cacheUTXO.clear();
        
//     m_touched += dev::eth::commit(m_cache, m_state);
//     m_changeLog.clear();
//     m_cache.clear();
//     m_unchangedCacheEntries.clear();
// }

void QtumState::kill(dev::Address _addr)
{
    // If the account is not in the db, nothing to kill.
    if (auto a = account(_addr))
        a->kill();
    if (auto v = vin(_addr))
        v->alive = 0;
}

void QtumState::addBalance(dev::Address const& _id, dev::u256 const& _amount)
{
    if (dev::eth::Account* a = account(_id))
    {
            // Log empty account being touched. Empty touched accounts are cleared
            // after the transaction, so this event must be also reverted.
            // We only log the first touch (not dirty yet), and only for empty
            // accounts, as other accounts does not matter.
            // TODO: to save space we can combine this event with Balance by having
            //       Balance and Balance+Touch events.
        if (!a->isDirty() && a->isEmpty())
            m_changeLog.emplace_back(dev::eth::detail::Change::Touch, _id);

            // Increase the account balance. This also is done for value 0 to mark
            // the account as dirty. Dirty account are not removed from the cache
            // and are cleared if empty at the end of the transaction.
        a->addBalance(_amount);
    }
    else
    {
        if(!addressInUse(newAddress) && newAddress != dev::Address()){
            const_cast<dev::Address&>(_id) = newAddress;
            newAddress = dev::Address();
        }
        createAccount(_id, {requireAccountStartNonce(), _amount});
    }

    if (_amount)
        m_changeLog.emplace_back(dev::eth::detail::Change::Balance, _id, _amount);
}

dev::Address QtumState::createQtumAddress(dev::h256 hashTx, uint32_t voutNumber){
    uint256 hashTXid(h256Touint(hashTx));
	std::vector<unsigned char> txIdAndVout(hashTXid.begin(), hashTXid.end());
	txIdAndVout.push_back(voutNumber);
		
	std::vector<unsigned char> SHA256TxVout(32);
    CSHA256().Write(txIdAndVout.data(), txIdAndVout.size()).Finalize(SHA256TxVout.data());

	std::vector<unsigned char> hashTxIdAndVout(20);
    CRIPEMD160().Write(SHA256TxVout.data(), SHA256TxVout.size()).Finalize(hashTxIdAndVout.data());
		
	return dev::Address(hashTxIdAndVout);
}

void QtumState::deleteAccounts(std::vector<dev::Address>& addrs){
    for(dev::Address addr : addrs){
        dev::eth::Account* acc = const_cast<dev::eth::Account*>(account(addr));
        acc->kill();
    }
}

void QtumState::updateUTXO(const std::unordered_map<dev::Address, Vin>& vins){
    for(auto& v : vins){
        Vin* vi = const_cast<Vin*>(vin(v.first));
        if(vi){
            vi->hash = v.second.hash;
            vi->nVout = v.second.nVout;
            vi->value = v.second.value;
            vi->alive = v.second.alive;
        } else if(v.second.alive > 0) {
            cacheUTXO[v.first] = v.second;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////
CTransaction CondensingTX::createCondensingTX(){
    selectionVin();
    calculatePlusAndMinus();
    if(!createNewBalances())
        return CTransaction();
    CMutableTransaction tx;
    tx.vin = createVins();;
    tx.vout = createVout();
    return CTransaction(tx);
}

std::unordered_map<dev::Address, Vin> CondensingTX::createVin(const CTransaction& tx){
    std::unordered_map<dev::Address, Vin> vins;
    for(auto& b : balances){
        if(b.first == transaction.sender())
            continue;

        if(b.second > 0){
            vins[b.first] = Vin{uintToh256(tx.GetHash()), nVouts[b.first], b.second, 1};
        } else {
            vins[b.first] = Vin{uintToh256(tx.GetHash()), 0, 0, 0};
        }
    }
    return vins;
}

void CondensingTX::selectionVin(){
    for(const TransferInfo& ti : transfers){
        if(!vins.count(ti.from)){
            if(auto a = state->vin(ti.from))
                vins[ti.from] = *a;
            if(ti.from == transaction.sender() && transaction.value() > 0){
                vins[ti.from] = Vin{transaction.getHashWith(), transaction.getNVout(), transaction.value(), 1};
            }
        }

        if(!vins.count(ti.to)){
            if(auto a = state->vin(ti.to))
                vins[ti.to] = *a;
        }
    }
}

void CondensingTX::calculatePlusAndMinus(){
    for(const TransferInfo& ti : transfers){
        if(!plusMinusInfo.count(ti.from)){
            plusMinusInfo[ti.from] = std::make_pair(0, ti.value);
        } else {
            plusMinusInfo[ti.from] = std::make_pair(plusMinusInfo[ti.from].first, plusMinusInfo[ti.from].second + ti.value);
        }

        if(!plusMinusInfo.count(ti.to)){
            plusMinusInfo[ti.to] = std::make_pair(ti.value, 0);
        } else {
            plusMinusInfo[ti.to] = std::make_pair(plusMinusInfo[ti.to].first + ti.value, plusMinusInfo[ti.to].second);
        }
    }
}

bool CondensingTX::createNewBalances(){
    for(auto& p : plusMinusInfo){
        dev::u256 balance = 0;
        if(vins.count(p.first)){
            balance = vins[p.first].value;
        }
        balance += p.second.first;
        if(balance < p.second.second)
            return false;
        balance -= p.second.second;
        balances[p.first] = balance;
    }
    return true;
}

std::vector<CTxIn> CondensingTX::createVins(){
    std::vector<CTxIn> ins;
    for(auto& v : vins){
        if(v.second.value > 0)
            ins.push_back(CTxIn(h256Touint(v.second.hash), v.second.nVout, CScript() << OP_TXHASH));
    }
    return ins;
}

std::vector<CTxOut> CondensingTX::createVout(){
    size_t count = 0;
    std::vector<CTxOut> outs;
    for(auto& b : balances){
        if(b.second > 0){
            CScript script;
            if(state->addressInUse(b.first)){
                script = CScript() << valtype{0} << valtype{0} << valtype{0} << valtype(1, 0) << b.first.asBytes() << OP_CALL;
            } else {
                script = CScript() << OP_DUP << OP_HASH160 << b.first.asBytes() << OP_EQUALVERIFY << OP_CHECKSIG;
            }
            outs.push_back(CTxOut(CAmount(b.second), script));
            nVouts[b.first] = count;
            count++;
        }
    }
    return outs;
}
///////////////////////////////////////////////////////////////////////////////////////////
