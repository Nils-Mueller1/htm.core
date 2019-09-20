/* ---------------------------------------------------------------------
 * HTM Community Edition of NuPIC
 * Copyright (C) 2016, Numenta, Inc.
 *               2019, David McDougall
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Affero Public License for more details.
 *
 * You should have received a copy of the GNU Affero Public License
 * along with this program.  If not, see http://www.gnu.org/licenses.
 * --------------------------------------------------------------------- */

#include <cmath> // exp
#include <numeric> // accumulate

#include <htm/algorithms/SDRClassifier.hpp>
#include <htm/utils/Log.hpp>

using namespace htm;
using namespace std;

UInt htm::argmax( const PDF & data )
  { return UInt( max_element( data.begin(), data.end() ) - data.begin() ); }


/******************************************************************************/

Classifier::Classifier(const Real alpha)
  { initialize( alpha ); }

void Classifier::initialize(const Real alpha)
{
  NTA_CHECK(alpha > 0.0f);
  alpha_ = alpha;
  dimensions_ = 0;
  categories_.clear();
  weights_.clear();
}


PDF Classifier::infer(const SDR & pattern) const {
  // Check input dimensions, or if this is the first time the Classifier is used and dimensions
  // are unset, return zeroes.
  NTA_CHECK( not categories_.empty() )
    << "Classifier: must call `learn` before `infer`.";
  NTA_ASSERT(pattern.size == dimensions_) << "Input SDR does not match previously seen size!";

  // Accumulate feed forward input.
  PDF probabilities( categories_.size(), 0.0f );
  for( const auto bit : pattern.getSparse() ) {
    for( size_t i=0u; i< categories_.size(); i++) {
      const auto category = categories_.at(i);
      probabilities[i] += weights_.at(bit).at(category); // needs .at() instead of [] because of the infer() const
    }
  }

  // Convert from accumulated votes to probability density function.
  softmax( probabilities.begin(), probabilities.end() );
  return probabilities;
}


void Classifier::learn(const SDR &pattern, const vector<UInt> &categoryIdxList)
{
  // If this is the first time the Classifier is being used, weights are empty, 
  // so we set the dimensions to that of the input `pattern`
  if( dimensions_ == 0 ) {
    dimensions_ = pattern.size;
    while( weights_.size() < pattern.size ) {
      std::unordered_map<UInt, Real64> initialEmptyWeights;
      weights_.push_back( initialEmptyWeights );
    }
  }
  NTA_ASSERT(pattern.size == dimensions_) << "Input SDR does not match previously seen size!";

  // Check if this is a new category & resize the weights table to hold it.
  for (const auto cat: categoryIdxList) {
    const bool alreadyInCategories = std::find(categories_.cbegin(), categories_.cend(), cat) != categories_.cend();
    if( not alreadyInCategories ) {
      categories_.push_back(cat);
      //update existing inner weights: set new cat's weight to zero
      for( auto & mapp : weights_ ) {
        mapp.insert({cat, 0.0f});
      }
    }
  }

  // Compute errors and update weights.
  const auto& error = calculateError_(categoryIdxList, pattern);
  for( const auto& bit : pattern.getSparse() ) {
    for(const auto cat: categories_) {
      weights_[bit][cat] += alpha_ * error[cat];
    }
  }
}


// Helper function to compute the error signal in learning.
std::vector<Real64> Classifier::calculateError_(const std::vector<UInt> &categoryIdxList, 
		                                const SDR &pattern) const {
  // compute predicted likelihoods
  auto likelihoods = infer(pattern);

  // Compute target likelihoods
  PDF targetDistribution(categories_.size() + 1u, 0.0f);
  for( size_t i = 0u; i < categoryIdxList.size(); i++ ) {
    targetDistribution[categoryIdxList[i]] = 1.0f / categoryIdxList.size();
  }

  for( size_t i = 0u; i < likelihoods.size(); i++ ) {
    likelihoods[i] = targetDistribution[i] - likelihoods[i];
  }
  return likelihoods;
}


void htm::softmax(PDF::iterator begin, PDF::iterator end) {
  if( begin == end ) {
    return;
  }
  const auto maxVal = *max_element(begin, end);
  for (auto itr = begin; itr != end; ++itr) {
    *itr = std::exp(*itr - maxVal); // x[i] = e ^ (x[i] - maxVal)
  }
  // Sum of all elements raised to exp(elem) each.
  const Real sum = (Real) std::accumulate(begin, end, 0.0f);
  NTA_ASSERT(sum > 0.0f);
  for (auto itr = begin; itr != end; ++itr) {
    *itr /= sum;
  }
}


/******************************************************************************/


Predictor::Predictor(const vector<StepsAheadT> &steps, const Real alpha)
  { initialize(steps, alpha); }


void Predictor::initialize(const vector<StepsAheadT> &steps, const Real alpha)
{
  NTA_CHECK( not steps.empty() ) << "Required argument steps is empty!";
  steps_ = steps;
  sort(steps_.begin(), steps_.end());

  for( const auto step : steps_ ) {
    classifiers_.emplace( step, alpha );
  }

  reset();
}


void Predictor::reset() {
  patternHistory_.clear();
  recordNumHistory_.clear();
}


Predictions Predictor::infer(const SDR &pattern) const {
  Predictions result;
  for( const auto step : steps_ ) {
    result.insert({step, classifiers_.at(step).infer( pattern )});
  }
  return result;
}


void Predictor::learn(const UInt recordNum, //TODO make recordNum optional, autoincrement as steps 
		      const SDR &pattern,
                      const std::vector<UInt> &bucketIdxList)
{
  checkMonotonic_(recordNum);

  // Update pattern history if this is a new record.
  const UInt lastRecordNum = recordNumHistory_.empty() ? 0 : recordNumHistory_.back();
  if (recordNumHistory_.size() == 0u || recordNum > lastRecordNum) {
    patternHistory_.emplace_back( pattern );
    recordNumHistory_.push_back(recordNum);
    if (patternHistory_.size() > steps_.back() + 1u) { //steps_ are sorted, so steps_.back() is the "oldest/deepest" N-th step (ie 10 of [1,2,10])
      patternHistory_.pop_front();
      recordNumHistory_.pop_front();
    }
  }

  // Iterate through all recently given inputs, starting from the furthest in the past.
  auto pastPattern   = patternHistory_.begin();
  auto pastRecordNum = recordNumHistory_.begin();
  for( ; pastRecordNum != recordNumHistory_.cend(); pastPattern++, pastRecordNum++ )
  {
    const UInt nSteps = recordNum - *pastRecordNum;

    // Update weights.
    if( binary_search( steps_.begin(), steps_.end(), nSteps )) {
      classifiers_.at(nSteps).learn( *pastPattern, bucketIdxList );
    }
  }
}


void Predictor::checkMonotonic_(const UInt recordNum) const {
  // Ensure that recordNum increases monotonically.
  const UInt lastRecordNum = recordNumHistory_.empty() ? 0 : recordNumHistory_.back();
  NTA_CHECK(recordNum >= lastRecordNum) << "The record number must increase monotonically.";
}
