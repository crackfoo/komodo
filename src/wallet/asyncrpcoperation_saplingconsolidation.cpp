// Copyright (c) 2020 The Hush developers
// Copyright (c) 2019 CryptoForge
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "assert.h"
#include "boost/variant/static_visitor.hpp"
#include "asyncrpcoperation_saplingconsolidation.h"
#include "init.h"
#include "key_io.h"
#include "rpc/protocol.h"
#include "random.h"
#include "sync.h"
#include "tinyformat.h"
#include "transaction_builder.h"
#include "util.h"
#include "utilmoneystr.h"
#include "wallet.h"

CAmount fConsolidationTxFee = DEFAULT_CONSOLIDATION_FEE;
bool fConsolidationMapUsed = false;
const int CONSOLIDATION_EXPIRY_DELTA = 15;

extern string randomSietchZaddr();

AsyncRPCOperation_saplingconsolidation::AsyncRPCOperation_saplingconsolidation(int targetHeight) : targetHeight_(targetHeight) {}

AsyncRPCOperation_saplingconsolidation::~AsyncRPCOperation_saplingconsolidation() {}

void AsyncRPCOperation_saplingconsolidation::main() {
    if (isCancelled())
        return;

    set_state(OperationStatus::EXECUTING);
    start_execution_clock();

    bool success = false;

    try {
        success = main_impl();
    } catch (const UniValue& objError) {
        int code = find_value(objError, "code").get_int();
        std::string message = find_value(objError, "message").get_str();
        set_error_code(code);
        set_error_message(message);
    } catch (const runtime_error& e) {
        set_error_code(-1);
        set_error_message("runtime error: " + string(e.what()));
    } catch (const logic_error& e) {
        set_error_code(-1);
        set_error_message("logic error: " + string(e.what()));
    } catch (const exception& e) {
        set_error_code(-1);
        set_error_message("general exception: " + string(e.what()));
    } catch (...) {
        set_error_code(-2);
        set_error_message("unknown error");
    }

    stop_execution_clock();

    if (success) {
        set_state(OperationStatus::SUCCESS);
    } else {
        set_state(OperationStatus::FAILED);
    }

    std::string s = strprintf("%s: Sapling Consolidation transaction created. (status=%s", getId(), getStateAsString());
    if (success) {
        s += strprintf(", success)\n");
    } else {
        s += strprintf(", error=%s)\n", getErrorMessage());
    }

    LogPrintf("%s", s);
}

bool AsyncRPCOperation_saplingconsolidation::main_impl() {
    bool status=true;
    auto opid=getId();
    LogPrint("zrpcunsafe", "%s: Beginning AsyncRPCOperation_saplingconsolidation.\n", opid);
    auto consensusParams = Params().GetConsensus();
    auto nextActivationHeight = NextActivationHeight(targetHeight_, consensusParams);
    if (nextActivationHeight && targetHeight_ + CONSOLIDATION_EXPIRY_DELTA >= nextActivationHeight.get()) {
        LogPrint("zrpcunsafe", "%s: Consolidation txs would be created before a NU activation but may expire after. Skipping this round.\n",opid);
        setConsolidationResult(0, 0, std::vector<std::string>());
        return status;
    }

    std::vector<CSproutNotePlaintextEntry> sproutEntries;
    std::vector<SaplingNoteEntry> saplingEntries;
    std::set<libzcash::SaplingPaymentAddress> addresses;
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        // We set minDepth to 11 to avoid unconfirmed notes and in anticipation of specifying
        // an anchor at height N-10 for each Sprout JoinSplit description
        // Consider, should notes be sorted?
        pwalletMain->GetFilteredNotes(sproutEntries, saplingEntries, "", 11);
        if (fConsolidationMapUsed) {
            const vector<string>& v = mapMultiArgs["-consolidatesaplingaddress"];
            for(int i = 0; i < v.size(); i++) {
                auto zAddress = DecodePaymentAddress(v[i]);
                if (boost::get<libzcash::SaplingPaymentAddress>(&zAddress) != nullptr) {
                    libzcash::SaplingPaymentAddress saplingAddress = boost::get<libzcash::SaplingPaymentAddress>(zAddress);
                    addresses.insert(saplingAddress );
                } else {
                    //TODO: how to handle invalid zaddrs?
                }
            }
        } else {
            pwalletMain->GetSaplingPaymentAddresses(addresses);
        }
    }

    int numTxCreated = 0;
    std::vector<std::string> consolidationTxIds;
    CAmount amountConsolidated = 0;
    CCoinsViewCache coinsView(pcoinsTip);

    for (auto addr : addresses) {
        libzcash::SaplingExtendedSpendingKey extsk;
        if (pwalletMain->GetSaplingExtendedSpendingKey(addr, extsk)) {

            std::vector<SaplingNoteEntry> fromNotes;
            CAmount amountToSend = 0;
            // max of 8 zins means the tx cannot reduce the anonset,
            // since there will be 8 zins and 8 zouts at worst case
            // This also helps reduce ztx creation time
            int maxQuantity = rand() % 8 + 1;
            for (const SaplingNoteEntry& saplingEntry : saplingEntries) {

                libzcash::SaplingIncomingViewingKey ivk;
                pwalletMain->GetSaplingIncomingViewingKey(boost::get<libzcash::SaplingPaymentAddress>(saplingEntry.address), ivk);

                //Select Notes from that same address we will be sending to.
                if (ivk == extsk.expsk.full_viewing_key().in_viewing_key()) {
                  amountToSend += CAmount(saplingEntry.note.value());
                  fromNotes.push_back(saplingEntry);
                }

                //Only use a randomly determined number of notes
                if (fromNotes.size() >= maxQuantity)
                  break;

            }

            // minimum required
            // We use 3 so that addresses can spent one zutxo and still have another zutxo to use while that
            // tx is confirming
            int minQuantity = 3;
            if (fromNotes.size() < minQuantity)
              continue;

            amountConsolidated += amountToSend;
            auto builder = TransactionBuilder(consensusParams, targetHeight_, pwalletMain);
            //builder.SetExpiryHeight(targetHeight_ + CONSOLIDATION_EXPIRY_DELTA);
            LogPrint("zrpcunsafe", "%s: Beginning creating transaction with Sapling output amount=%s\n", opid, FormatMoney(amountToSend - fConsolidationTxFee));

            // Select Sapling notes
            std::vector<SaplingOutPoint> ops;
            std::vector<libzcash::SaplingNote> notes;
            for (auto fromNote : fromNotes) {
                ops.push_back(fromNote.op);
                notes.push_back(fromNote.note);
            }

            // Fetch Sapling anchor and witnesses
            uint256 anchor;
            std::vector<boost::optional<SaplingWitness>> witnesses;
            {
                LOCK2(cs_main, pwalletMain->cs_wallet);
                pwalletMain->GetSaplingNoteWitnesses(ops, witnesses, anchor);
            }

            // Add Sapling spends
            for (size_t i = 0; i < notes.size(); i++) {
                if (!witnesses[i]) {
                    LogPrint("zrpcunsafe", "%s: Missing Witnesses. Stopping.\n", opid);
                    status=false;
                    break;
                }
                builder.AddSaplingSpend(extsk.expsk, notes[i], anchor, witnesses[i].get());
            }

            builder.SetFee(fConsolidationTxFee);

            // Add the actual consolidation tx
            builder.AddSaplingOutput(extsk.expsk.ovk, addr, amountToSend - fConsolidationTxFee);
            LogPrint("zrpcunsafe", "%s: Added consolidation output %s", opid, addr.GetHash().ToString().c_str() );


            // Add sietch zouts
            int MIN_ZOUTS = 7;
            for(size_t i = 0; i < MIN_ZOUTS; i++) {
                // In Privacy Zdust We Trust -- Duke
                string zdust = randomSietchZaddr();
                LogPrint("zrpcunsafe", "%s: random zdust=%s", opid, zdust);
                auto zaddr   = DecodePaymentAddress(zdust);
                if (IsValidPaymentAddress(zaddr)) {
                    auto sietchZoutput = boost::get<libzcash::SaplingPaymentAddress>(zaddr);
                    LogPrint("zrpcunsafe", "%s: Adding OLD sietch output %d %s", opid, i, sietchZoutput.GetHash().ToString().c_str() );
                    CAmount amount=0;

                    // actually add our sietch zoutput, the new way
                    builder.AddSaplingOutput(extsk.expsk.ovk, sietchZoutput, amount);
                } else {
                    LogPrint("zrpcunsafe", "%s: Invalid payment address! Stopping.", opid);
                    status = false;
                    break;
                }
            }
            LogPrint("zrpcunsafe", "%s: Done adding %d sietch zouts", opid, MIN_ZOUTS);
            //CTransaction tx = builder.Build();

            auto maybe_tx = builder.Build();
            if (!maybe_tx) {
                LogPrint("zrpcunsafe", "%s: Failed to build transaction.",opid);
                status=false;
                break;
            }
            CTransaction tx = maybe_tx.get();

            if (isCancelled()) {
                LogPrint("zrpcunsafe", "%s: Canceled. Stopping.\n", opid);
                status=false;
                break;
            }

            if(pwalletMain->CommitConsolidationTx(tx)) {
                LogPrint("zrpcunsafe", "%s: Committed consolidation transaction with txid=%s\n",opid, tx.GetHash().ToString());
                amountConsolidated += amountToSend - fConsolidationTxFee;
                consolidationTxIds.push_back(tx.GetHash().ToString());
            } else {
                LogPrint("zrpcunsafe", "%s: Consolidation transaction FAILED in CommitTransaction, txid=%s\n",opid , tx.GetHash().ToString());
                setConsolidationResult(numTxCreated, amountConsolidated, consolidationTxIds);
                status = false;
                break;
            }

        }
    }

    LogPrint("zrpcunsafe", "%s: Created %d transactions with total Sapling output amount=%s,status=%d\n",opid , numTxCreated, FormatMoney(amountConsolidated), (int)status);
    setConsolidationResult(numTxCreated, amountConsolidated, consolidationTxIds);
    return status;
}

void AsyncRPCOperation_saplingconsolidation::setConsolidationResult(int numTxCreated, const CAmount& amountConsolidated, const std::vector<std::string>& consolidationTxIds) {
    UniValue res(UniValue::VOBJ);
    res.push_back(Pair("num_tx_created", numTxCreated));
    res.push_back(Pair("amount_consolidated", FormatMoney(amountConsolidated)));
    UniValue txIds(UniValue::VARR);
    for (const std::string& txId : consolidationTxIds) {
        txIds.push_back(txId);
    }
    res.push_back(Pair("consolidation_txids", txIds));
    set_result(res);
}

void AsyncRPCOperation_saplingconsolidation::cancel() {
    set_state(OperationStatus::CANCELLED);
}

UniValue AsyncRPCOperation_saplingconsolidation::getStatus() const {
    UniValue v = AsyncRPCOperation::getStatus();
    UniValue obj = v.get_obj();
    obj.push_back(Pair("method", "saplingconsolidation"));
    obj.push_back(Pair("target_height", targetHeight_));
    return obj;
}
