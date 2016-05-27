/*
   This file is part of the Eagle haplotype phasing software package
   developed by Po-Ru Loh.  Copyright (C) 2015-2016 Harvard University.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <vector>
#include <iostream>
#include <map>
#include <utility>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cassert>

#include <boost/random.hpp>
#include <boost/random/lagged_fibonacci.hpp>
#include <boost/random/uniform_01.hpp>

#include "HapHedge.hpp"
#include "NumericUtils.hpp"
#include "Timer.hpp"
#include "DipTreePBWT.hpp"

namespace EAGLE {

  using std::vector;
  using std::cout;
  using std::endl;

  const int TO_UNKNOWN = -2, TO_NONE = -1; // TO_NONE used in HapPathSplit and HapPrefix


  // struct HapPathSplit

  HapPathSplit::HapPathSplit(void) {};
  HapPathSplit::HapPathSplit(int _t) : t(_t), relProbLastStop(1), hapPrefixInd(0) {
    hapPrefixTo[0] = hapPrefixTo[1] = TO_UNKNOWN;
  };
  HapPathSplit::HapPathSplit(int _t, float relProb, int ind)
    : t(_t), relProbLastStop(relProb), hapPrefixInd(ind) {
    hapPrefixTo[0] = hapPrefixTo[1] = TO_UNKNOWN;
  };


  // struct HapPath

  HapPath::HapPath(void) {};


  // struct HapPrefix

  HapPrefix::HapPrefix(void) {};
  HapPrefix::HapPrefix(const HapTreeState &_state) {
    state = _state;
    //to[0] = to[1] = TO_UNKNOWN;
  };

  // class HapWaves

  HapWaves::HapWaves(const HapHedgeErr &_hapHedge, const vector <float> &_cMcoords,
		     int _histLength, int _beamWidth, float _logPerr, int _tCur) :
    rng(123), rand01(rng, boost::uniform_01<>()),
    hapHedge(_hapHedge), cMcoords(_cMcoords), histLength(_histLength),
    beamWidth(_beamWidth), pErr(expf(_logPerr)), maxHapPaths(2*beamWidth),
    maxHapPrefixes(maxHapPaths*histLength*2+1), tCur(_tCur) {

    curMod = tCur % HAPWAVES_HIST; nextMod = (tCur+1) % HAPWAVES_HIST;

    for (int p = 0; p < HAPWAVES_HIST; p++) {
      hapPathSizes[p] = 0;
      hapPaths[p] = new HapPath[maxHapPaths];
      for (int i = 0; i < maxHapPaths; i++)
	hapPaths[p][i].splitList = new HapPathSplit[histLength];
      hapPrefixes[p] = new HapPrefix[maxHapPrefixes];
    }

    // add root of 0th HapTree as cur HapPath
    hapPaths[curMod][0].cumLogP = 0;
    hapPaths[curMod][0].splitListLength = 1;
    hapPaths[curMod][0].splitList[0] = HapPathSplit(tCur);
    hapPaths[curMod][0].to[0] = hapPaths[0][0].to[1] = TO_UNKNOWN;
    hapPathSizes[curMod] = 1;
      
    hapPrefixes[curMod][0] = HapPrefix(hapHedge.getHapTreeMulti(tCur).getRootState());
    hapPrefixSizes[curMod] = 1;

  }

  HapWaves::~HapWaves(void) {
    for (int p = 0; p < HAPWAVES_HIST; p++) {
      for (int i = 0; i < maxHapPaths; i++)
	delete[] hapPaths[p][i].splitList;
      delete[] hapPaths[p];
      delete[] hapPrefixes[p];
    }
  }

  float recombP(int tCur, int tSplit, const vector <float> &cMcoords) {
    if (tCur+1 == (int) cMcoords.size()) return 1.0f;
    else {
      const float cMpseudo = 2.0f, minRecombP = 0.000001f, maxRecombP = 1.0f;//pErr;
      return std::max(std::min(3 * (cMcoords[tCur+1]-cMcoords[tCur])
			       / (cMpseudo + 2*(cMcoords[tCur+1]-cMcoords[tSplit])),
			       maxRecombP), minRecombP);
    }
  }

  // populate hapPrefixes[nextMod]
  // populate toCumLogP[] in hapPaths[curMod] (but don't populate hapPaths[nextMod])
  void HapWaves::computeAllExtensions(const vector <uchar> &nextPossibleBits) {
    // add root of next (= new cur) HapTree as beginning of HapPrefix list
    if (tCur+1 < (int) cMcoords.size()) {
      hapPrefixes[nextMod][0] = HapPrefix(hapHedge.getHapTreeMulti(tCur+1).getRootState());
      hapPrefixSizes[nextMod] = 1;
    }
    
    float mult = hapHedge.getHapTreeMulti(tCur).getInvNhaps();

    // iterate over paths
    for (int i = 0; i < hapPathSizes[curMod]; i++) {
      float relProbStopNext[2] = {0, 0};
      // iterate over splits
      for (int j = 0; j < hapPaths[curMod][i].splitListLength; j++) {
	HapPathSplit &split = hapPaths[curMod][i].splitList[j];
	// iterate over next possible bits
	for (int b = 0; b < 2; b++) {
	  if (!((nextPossibleBits[i]>>b)&1)) continue;
	  HapPrefix &hapPrefix = hapPrefixes[curMod][split.hapPrefixInd];
	  // if extension of hap prefix hasn't been attempted, attempt to perform extension
	  if (split.hapPrefixTo[b] == TO_UNKNOWN) {
	    split.hapPrefixTo[b] = TO_NONE; // default: can't extend (overwrite if path found)
	    hapPrefix.toHetOnlyProb[b] = 0;
	    // try to extend hap prefix:
	    // fill in split.hapPrefixTo[b], hapPrefixes[curMod][split.hapPrefixInd].to*[b]
	    const HapTreeMulti &hapTree = hapHedge.getHapTreeMulti(split.t);

	    HapTreeState state = hapPrefix.state;
	    if (hapTree.next(2*tCur, state, b)) { // can extend to match at het
	      hapPrefix.toHetOnlyProb[b] += mult * state.count;
	      if (hapTree.next(2*tCur+1, state, 0)) { // no err in inter-het region
		// create and link new HapPrefix node in hapPrefixes[nextMod]; link
		split.hapPrefixTo[b] = hapPrefixSizes[nextMod]++;
		hapPrefixes[nextMod][split.hapPrefixTo[b]].state = state;
	      }
	    }
	  }
	  relProbStopNext[b] += split.relProbLastStop * hapPrefix.toHetOnlyProb[b]
	    * recombP(tCur, split.t, cMcoords);
	}
      }
      for (int b = 0; b < 2; b++) {
	if (!((nextPossibleBits[i]>>b)&1)) continue;
	float relLogP = -1000;
	if (relProbStopNext[b] != 0) relLogP = logf(relProbStopNext[b]);
	hapPaths[curMod][i].toCumLogP[b] =
	  hapPaths[curMod][i].cumLogP + relLogP;// + recombLogPs[tCur];
      }
    }
  }

  float HapWaves::getToCumLogProb(int ind, int nextBit) const {
    return hapPaths[curMod][ind].toCumLogP[nextBit];
  }

  // look up/create extension of hapPaths[curMod][ind] in hapPaths[nextMod]
  // return index in hapPaths[nextMod]
  int HapWaves::extendPath(int ind, int nextBit) {
    HapPath &curHapPath = hapPaths[curMod][ind];
    if (curHapPath.to[nextBit] == TO_UNKNOWN) {
      int nextInd = hapPathSizes[nextMod]++;
      assert(hapPathSizes[nextMod]<=maxHapPaths);
      curHapPath.to[nextBit] = nextInd;
      HapPath &nextHapPath = hapPaths[nextMod][nextInd];
      nextHapPath.cumLogP = curHapPath.toCumLogP[nextBit];
      float calibP = expf(curHapPath.cumLogP - nextHapPath.cumLogP);
      int &nSplit = nextHapPath.splitListLength; nSplit = 0;
      nextHapPath.to[0] = nextHapPath.to[1] = TO_UNKNOWN;
      for (int j = (curHapPath.splitList[0].t + histLength == tCur+1 ? 1 : 0);
	   j < curHapPath.splitListLength; j++) {
	const HapPathSplit &curSplit = curHapPath.splitList[j];
	if (curSplit.hapPrefixTo[nextBit] != TO_NONE) {
	  nextHapPath.splitList[nSplit++] = HapPathSplit(curSplit.t,
							 curSplit.relProbLastStop * calibP,
							 curSplit.hapPrefixTo[nextBit]);
	}
      }
      nextHapPath.splitList[nSplit++] = HapPathSplit(tCur+1); // restart
    }
    return curHapPath.to[nextBit];
  }
    
  void HapWaves::advance(void) {
    tCur++; curMod = tCur % HAPWAVES_HIST; nextMod = (tCur+1) % HAPWAVES_HIST;
    hapPathSizes[nextMod] = 0;
    hapPrefixSizes[nextMod] = 0;
  }

  void HapWaves::sampleLastPrefix(int &tStart, HapTreeState &state, int t, int hapPathInd,
				  int tBit) {
    assert(tCur+1 - t < HAPWAVES_HIST);
    int tMod = t % HAPWAVES_HIST;
    const HapPath &hapPath = hapPaths[tMod][hapPathInd];
    
    float relProbStopNext = 0;

    vector <float> cumRelProbStopNext(hapPath.splitListLength);
    for (int j = 0; j < hapPath.splitListLength; j++) {
      const HapPathSplit &split = hapPath.splitList[j];
      const HapPrefix &hapPrefix = hapPrefixes[tMod][split.hapPrefixInd];
      relProbStopNext += split.relProbLastStop * hapPrefix.toHetOnlyProb[tBit]
	* recombP(t, split.t, cMcoords);
      cumRelProbStopNext[j] = relProbStopNext;
    }

    float relLogP = -1000;
    if (relProbStopNext != 0) relLogP = logf(relProbStopNext);
    assert(hapPaths[tMod][hapPathInd].toCumLogP[tBit] == hapPath.cumLogP + relLogP);

    float r = rand01();

    for (int j = 0; j < hapPath.splitListLength; j++)
      if (cumRelProbStopNext[j]/relProbStopNext > r || j+1 == hapPath.splitListLength) {
	const HapPathSplit &split = hapPath.splitList[j];
	const HapPrefix &hapPrefix = hapPrefixes[tMod][split.hapPrefixInd];
	tStart = split.t;
	state = hapPrefix.state;
	return;
      }
  }


  // struct DipTreeNode

  bool DipTreeNode::operator < (const DipTreeNode &dNode) const {
    return logP+boostLogP > dNode.logP+dNode.boostLogP;
  }


  // class DipTree

  void DipTree::traceNode(int t, int i) {
    int from = nodes[t][i].from;
    if (t>1) traceNode(t-1, from);
    cout << "(" << (int) nodes[t][i].hapMat << "," << (int) nodes[t][i].hapPat << ") ";
  }

  std::pair <uint64, uint64> truncPair(uint64 histMat, uint64 histPat, uint64 histBits) {
    uint64 mask = histBits>=64ULL ? -1ULL : (1ULL<<histBits)-1;
    uint64 x = histMat&mask, y = histPat&mask;
    return x<y ? std::make_pair(x, y) : std::make_pair(y, x);
  }

  void DipTree::advance(void) {

    bool isOppConstrained = constraints[tCur]==OPP_CONSTRAINT; // constrained to be 0|1 or 1|0
    bool isFullyConstrained = !isOppConstrained && constraints[tCur]!=NO_CONSTRAINT;
    
    // populate next possible bits: nextPossibleBits[i] corresponds to hapPaths[curMod][i]
    //                              for i = dNode.hapPathInds[0], dNode.hapPathInds[1]
    vector <uchar> nextPossibleBits(2*beamWidth);
    int checkWidth = std::min((int) nodes[tCur].size(), beamWidth);
    vector <char> reqMats(checkWidth), reqPats(checkWidth);
    const float logPthresh = 2*logPerr;//logf(0.000001f);
    for (int i = 0; i < checkWidth; i++) {
      const DipTreeNode &dNode = nodes[tCur][i];
      if (dNode.logP+dNode.boostLogP < nodes[tCur][0].logP+nodes[tCur][0].boostLogP + logPthresh) {
	checkWidth = i;
	break;
      }
      assert(dNode.hapPathInds[0] < (int) nextPossibleBits.size());
      assert(dNode.hapPathInds[1] < (int) nextPossibleBits.size());
      if (isFullyConstrained) {
	char &reqMat = reqMats[i], &reqPat = reqPats[i];
	if ((constraints[tCur]>>1) == 0) // no-hom-err constraint
	  reqMat = reqPat = constraints[tCur]&1;
	else { // rel phase constraint
	  int t = tCur, ind = i;
	  for (int d = 0; d < (constraints[tCur]>>1)-1; d++)
	    ind = nodes[t--][ind].from;
	  reqMat = nodes[t][ind].hapMat ^ (constraints[tCur]&1);
	  reqPat = nodes[t][ind].hapPat ^ (constraints[tCur]&1);
	}
	nextPossibleBits[dNode.hapPathInds[0]] |= 1<<reqMat;
	nextPossibleBits[dNode.hapPathInds[1]] |= 1<<reqPat;
      }
      else {
	nextPossibleBits[dNode.hapPathInds[0]] = 3;
	nextPossibleBits[dNode.hapPathInds[1]] = 3;
      }
    }
    // extend hap paths (part 1)
    hapWaves.computeAllExtensions(nextPossibleBits);
    
    // extend dip paths
    vector <DipTreeNode> nextNodes;
    for (int i = 0; i < checkWidth; i++) {
      const DipTreeNode &dNode = nodes[tCur][i];
      for (char hapMat = 0; hapMat < 2; hapMat++)
	for (char hapPat = 0; hapPat < 2; hapPat++) {
	  if (!dNode.unequalAnc && hapMat > hapPat) continue;
	  if (isFullyConstrained && (hapMat != reqMats[i] || hapPat != reqPats[i])) continue;
	  if (isOppConstrained && hapMat==hapPat) continue;
	  DipTreeNode nextNode;
	  nextNode.from = i;
	  nextNode.unequalAnc = dNode.unequalAnc || (hapMat != hapPat);
	  nextNode.hapMat = hapMat;
	  nextNode.hapPat = hapPat;
	  nextNode.numErr = dNode.numErr + (genos[tCur]<=2 && hapMat+hapPat != genos[tCur]);
	  nextNode.logP = hapWaves.getToCumLogProb(dNode.hapPathInds[0], hapMat) +
	    hapWaves.getToCumLogProb(dNode.hapPathInds[1], hapPat) + nextNode.numErr * logPerr;
	  nextNode.boostLogP = dNode.boostLogP;
	  if (isFullyConstrained) {
	    nextNode.histMat = dNode.histMat;
	    nextNode.histPat = dNode.histPat;
	  }
	  else {
	    nextNode.histMat = (dNode.histMat<<1ULL) | hapMat;
	    nextNode.histPat = (dNode.histPat<<1ULL) | hapPat;
	  }
	  nextNodes.push_back(nextNode);
	}
    }

    if (!isFullyConstrained) {
      // compute number of bits of history to use (histLength minus # of fully constrained sites)
      int histBits = 0;
      for (int t = tCur; t > std::max(tCur-histLength, 0); t--)
	if (constraints[t]==OPP_CONSTRAINT || constraints[t]==NO_CONSTRAINT)
	  histBits++;

      // aggregate DipTree paths that agree exactly in past histLength
      std::sort(nextNodes.begin(), nextNodes.end());
      std::map < std::pair <uint64, uint64>, int > histToInd;
      for (int i = 0; i < (int) nextNodes.size(); i++) {
	const DipTreeNode &nextNode = nextNodes[i];
	std::pair <uint64, uint64> histPair =
	  truncPair(nextNode.histMat, nextNode.histPat, histBits);
	std::map < std::pair <uint64, uint64>, int >::iterator it = histToInd.find(histPair);
	if (it == histToInd.end()) {
	  histToInd[histPair] = nodes[tCur+1].size();
	  nodes[tCur+1].push_back(nextNode);
	}
	else {
	  int j = it->second;
	  float sumLogPj = nodes[tCur+1][j].logP + nodes[tCur+1][j].boostLogP;
	  float sumLogPi = nextNode.logP + nextNode.boostLogP;
	  NumericUtils::logSumExp(sumLogPi, sumLogPj); // prob i += prob existing tCur+1 node j
	  nodes[tCur+1][j].boostLogP += sumLogPi - sumLogPj; // augment boost for existing node j
	}
      }
      //cout << " " << nodes[tCur+1].size() << "/" << nextNodes.size() << std::flush;
    }
    else
      nodes[tCur+1] = nextNodes;
    
    // extend hap paths of top beamWidth DipTree nodes (part 2)
    for (int i = 0; i < std::min((int) nodes[tCur+1].size(), beamWidth); i++) {
      DipTreeNode &nextNode = nodes[tCur+1][i];
      const DipTreeNode &dNode = nodes[tCur][nextNode.from];
      nextNode.hapPathInds[0] = hapWaves.extendPath(dNode.hapPathInds[0], nextNode.hapMat);
      nextNode.hapPathInds[1] = hapWaves.extendPath(dNode.hapPathInds[1], nextNode.hapPat);
    }

    hapWaves.advance();
    tCur++;

    float totLogP = nodes[tCur][0].logP + nodes[tCur][0].boostLogP;
    for (int i = 1; i < (int) nodes[tCur].size(); i++)
      NumericUtils::logSumExp(totLogP, nodes[tCur][i].logP + nodes[tCur][i].boostLogP);
    for (int i = 0; i < (int) nodes[tCur].size(); i++) {
      normProbs[tCur].push_back(expf(nodes[tCur][i].logP + nodes[tCur][i].boostLogP - totLogP));
      //traceNode(tCur, i); cout << normProbs[tCur].back() << endl;
    }
  }

  DipTree::DipTree(const HapHedgeErr &_hapHedge, const vector <uchar> &_genos,
		   const char *_constraints, const vector <float> &_cMcoords,
		   int _histLength, int _beamWidth, float _logPerr, int _tCur) :
    rng(12345), rand01(rng, boost::uniform_01<>()),
    hapHedge(_hapHedge), hapWaves(_hapHedge, _cMcoords, _histLength, _beamWidth, _logPerr, _tCur),
    genos(_genos), constraints(_constraints), histLength(_histLength), beamWidth(_beamWidth),
    logPerr(_logPerr), tCur(_tCur), T(_hapHedge.getNumTrees()), nodes(T+1), normProbs(T+1) {

    DipTreeNode dNode;
    dNode.from = -1; dNode.unequalAnc = 0; dNode.logP = 0; dNode.numErr = 0;
    dNode.hapPathInds[0] = dNode.hapPathInds[1] = 0;
    dNode.histMat = 0; dNode.histPat = 0; dNode.boostLogP = 0;
    nodes[tCur].push_back(dNode); // root of DipTree
  }

  // compute probability of AA at hets tCallLoc1 and tCallLoc2
  float DipTree::callProbAA(int tCallLoc1, int tCallLoc2, int callLength) {
    assert(tCallLoc1>0 && tCallLoc2<T);
    int tFront = std::min(T, tCallLoc2 + callLength);
    while (tCur < tFront)
      advance();
    float probAA = 0, probAB = 0;
    for (int i = 0; i < (int) nodes[tFront].size(); i++) {
      int t = tFront, ind = i;
      while (t != tCallLoc2+1)
	ind = nodes[t--][ind].from;
      char hapMat2 = nodes[t][ind].hapMat, hapPat2 = nodes[t][ind].hapPat; // alleles at tCallLoc2
      while (t != tCallLoc1+1)
	ind = nodes[t--][ind].from;
      char hapMat1 = nodes[t][ind].hapMat, hapPat1 = nodes[t][ind].hapPat; // alleles at tCallLoc1
      if (hapMat2 != hapPat2 && hapMat1 != hapPat1) {
	if (hapMat1 == hapMat2)
	  probAA += normProbs[tFront][i];
	else
	  probAB += normProbs[tFront][i];
      }
      else {
	probAA += normProbs[tFront][i] / 2;
	probAB += normProbs[tFront][i] / 2;
      }
    }
    if (probAA + probAB == 0) return 0.5;
    return probAA / (probAA + probAB);
  }

  // compute diploid dosage at tCallLoc
  float DipTree::callDosage(int tCallLoc, int callLength) {
    assert(tCallLoc>0 && tCallLoc<T);
    int tFront = std::min(T, tCallLoc + callLength);
    while (tCur < tFront)
      advance();
    float prob1 = 0, probTot = 0;
    for (int i = 0; i < (int) nodes[tFront].size(); i++) {
      int t = tFront, ind = i;
      while (t != tCallLoc+1)
	ind = nodes[t--][ind].from;
      char hapMat = nodes[t][ind].hapMat, hapPat = nodes[t][ind].hapPat; // alleles at tCallLoc
      prob1 += (hapMat+hapPat) * normProbs[tFront][i];
      probTot += normProbs[tFront][i];
    }
    if (probTot == 0) return 1.0;
    return prob1 / probTot;
  }

  vector < pair <int, int> > DipTree::sampleRefs(int tCallLoc, int callLength, int samples) {
    assert(tCallLoc>0 && tCallLoc<T);
    int tFront = std::min(T, tCallLoc + callLength);
    while (tCur < tFront)
      advance();

    float probTot = 0;
    for (int i = 0; i < (int) nodes[tFront].size(); i++)
      probTot += normProbs[tFront][i];

    vector < std::pair <int, int> > ret;

    int lengths[samples][2];
    for (int s = 0; s < samples; s++) {
      float r = rand01();
      float cumProb = 0;
      for (int i = 0; i < (int) nodes[tFront].size(); i++) {
	cumProb += normProbs[tFront][i] / probTot;
	if (cumProb > r || i+1 == (int) nodes[tFront].size()) {
	  int refs[2] = {-1, -1};
	  for (int h = 0; h < 2; h++) {
	    int t = tFront, tStart = tFront;
	    int ind = i;
	    HapTreeState state; int tBit = 0;
	    while (tCallLoc < tStart) {
	      while (t != tStart) // rewind DipTree from t to tStart
		ind = nodes[t--][ind].from;
	      tBit = h==0 ? nodes[t][ind].hapMat : nodes[t][ind].hapPat; // allele at tStart-1
	      ind = nodes[t--][ind].from; // move t back 1; now tBit is allele at t = tStart-1
	      hapWaves.sampleLastPrefix(tStart, state, t, nodes[t][ind].hapPathInds[h], tBit);
	      lengths[s][h] = t-tStart;
	    }
	    const HapTreeMulti &hapTree = hapHedge.getHapTreeMulti(tStart);
	    assert(hapTree.next(2*t, state, tBit)); // extend state to bit=tBit @ t

	    // randomly sample from this prefix, moving up to 10 hets ahead
	    for (int m = 2*t+1; m < 2*T && m < 2*t+20; m++) {
	      if (m % 2 == 1) // error bit: extend to 0-err hom region if possible
		hapTree.nextAtFrac(m, state, 0);
	      else // het bit: randomly choose extension
		hapTree.nextAtFrac(m, state, rand01());
	    }

	    int refSeq = state.seq; 

	    const HapBitsT &hapBitsT = hapHedge.getHapBitsT();
	    /*
	    // check tStart..t of refSeq matches geno
	    for (int m = 2*tStart+1; m <= 2*t; m += 2)
	      assert(hapBitsT.getBit(refSeq, m)==0);
	    */
	    refs[hapBitsT.getBit(refSeq, 2*tCallLoc)] = refSeq;
	  }
	  if (refs[0] != -1 && refs[1] != -1) // no genotype error; one parent with each allele
	    ret.push_back(make_pair(refs[0], refs[1]));
	  break;
	}
      }
    }
    /*
    if (tCallLoc % 100 == 0) {
      for (int h = 0; h < 2; h++) {
	for (int s = 0; s < samples; s++)
	  cout << lengths[s][h] << " ";
	cout << "     ";
      }
      cout << endl;
    }
    */
    return ret;
  }

}
