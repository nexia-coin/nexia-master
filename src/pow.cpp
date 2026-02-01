// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    if (static_cast<uint32_t>(pindexLast->nHeight + 1) >= 110) {
        return LwmaCalculateNextWorkRequired(pindexLast, params);
    } else {

      // Only change once per difficulty adjustment interval
      if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
      {
          if (params.fPowAllowMinDifficultyBlocks)
          {
              // Special difficulty rule for testnet:
              // If the new block's timestamp is more than 2* 10 minutes
              // then allow mining of a min-difficulty block.
              if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                  return nProofOfWorkLimit;
              else
              {
                  // Return the last non-special-min-difficulty-rules-block
                  const CBlockIndex* pindex = pindexLast;
                  while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                      pindex = pindex->pprev;
                  return pindex->nBits;
              }
          }
          return pindexLast->nBits;
      }

      // Go back by what we want to be 14 days worth of blocks
      // Nexia: This fixes an issue where a 51% attack can change difficulty at will.
      // Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
      int blockstogoback = params.DifficultyAdjustmentInterval()-1;
      if ((pindexLast->nHeight+1) != params.DifficultyAdjustmentInterval())
          blockstogoback = params.DifficultyAdjustmentInterval();

      // Go back by what we want to be 14 days worth of blocks
      const CBlockIndex* pindexFirst = pindexLast;
      for (int i = 0; pindexFirst && i < blockstogoback; i++)
          pindexFirst = pindexFirst->pprev;

      assert(pindexFirst);

      return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
    }
}

unsigned int LwmaCalculateNextWorkRequired(const CBlockIndex* pindexLast, const Consensus::Params& params)
{
    const int64_t T = params.nPowTargetSpacing;
    const int64_t N_full = 96;
    const int64_t height = pindexLast->nHeight;
    const arith_uint256 powLimit = UintToArith256(params.powLimit);

    int64_t N = std::min<int64_t>(N_full, height);
    if (N <= 0) return powLimit.GetCompact();
    const int64_t k = N * (N + 1) * T / 2;

    arith_uint256 sumTarget, nextTarget;
    int64_t thisTimestamp, previousTimestamp;
    int64_t t = 0, j = 0;

    const CBlockIndex* blockPreviousTimestamp = pindexLast->GetAncestor(height - N);
    previousTimestamp = blockPreviousTimestamp->GetBlockTime();

    const CBlockIndex* pindexTransition = nullptr;
    arith_uint256 transitionTarget;
    
    if (params.LWMAHeight > 0 && height >= params.LWMAHeight - 1) {
         pindexTransition = pindexLast->GetAncestor(params.LWMAHeight - 1);
         if (pindexTransition) {
             transitionTarget.SetCompact(pindexTransition->nBits);
         }
    }

    for (int64_t i = height - N + 1; i <= height; i++) {
        const CBlockIndex* block = pindexLast->GetAncestor(i);
        thisTimestamp = std::max<int64_t>(block->GetBlockTime(), previousTimestamp + 1);

        int64_t solvetime = std::min<int64_t>(6 * T, thisTimestamp - previousTimestamp);
        previousTimestamp = thisTimestamp;

        j++;
        t += solvetime * j; // Weighted solvetime sum.
        
        arith_uint256 target;
        
        if (i < params.LWMAHeight && pindexTransition != nullptr) {
            target = transitionTarget;
        } else {
            target.SetCompact(block->nBits);
        }

        sumTarget += target / N;
    }
    nextTarget = (sumTarget * t) / k;

    if (nextTarget > powLimit) { nextTarget = powLimit; }

    return nextTarget.GetCompact();
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    arith_uint256 bnNew;
    arith_uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    // Nexia: intermediate uint256 can overflow by 1 bit
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    bool fShift = bnNew.bits() > bnPowLimit.bits() - 1;
    if (fShift)
        bnNew >>= 1;
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;
    if (fShift)
        bnNew <<= 1;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits)
{

    if (height + 1 >= static_cast<int64_t>(params.LWMAHeight)) return true;

    if (params.fPowAllowMinDifficultyBlocks) return true;

    if (height % params.DifficultyAdjustmentInterval() == 0) {
        int64_t smallest_timespan = params.nPowTargetTimespan/4;
        int64_t largest_timespan = params.nPowTargetTimespan*4;

        const arith_uint256 pow_limit = UintToArith256(params.powLimit);
        arith_uint256 observed_new_target;
        observed_new_target.SetCompact(new_nbits);

        // Calculate the largest difficulty value possible:
        arith_uint256 largest_difficulty_target;
        largest_difficulty_target.SetCompact(old_nbits);
        largest_difficulty_target *= largest_timespan;
        largest_difficulty_target /= params.nPowTargetTimespan;

        if (largest_difficulty_target > pow_limit) {
            largest_difficulty_target = pow_limit;
        }

        arith_uint256 maximum_new_target;
        maximum_new_target.SetCompact(largest_difficulty_target.GetCompact());
        if (maximum_new_target < observed_new_target) return false;

        arith_uint256 smallest_difficulty_target;
        smallest_difficulty_target.SetCompact(old_nbits);
        smallest_difficulty_target *= smallest_timespan;
        smallest_difficulty_target /= params.nPowTargetTimespan;

        if (smallest_difficulty_target > pow_limit) {
            smallest_difficulty_target = pow_limit;
        }

        arith_uint256 minimum_new_target;
        minimum_new_target.SetCompact(smallest_difficulty_target.GetCompact());
        if (minimum_new_target > observed_new_target) return false;
    } else if (old_nbits != new_nbits) {
        return false;
    }
    return true;
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
