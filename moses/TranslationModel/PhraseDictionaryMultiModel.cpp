/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2006 University of Edinburgh

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
***********************************************************************/
#include "util/exception.hh"

#include "moses/TranslationModel/PhraseDictionaryMultiModel.h"

using namespace std;

namespace Moses

{
PhraseDictionaryMultiModel::PhraseDictionaryMultiModel(const std::string &line)
  :PhraseDictionary("PhraseDictionaryMultiModel", line)
{
  size_t ind = 0;
  while (ind < m_args.size()) {
    vector<string> &args = m_args[ind];
    bool consumed = SetParameter(args[0], args[1]);
    if (consumed) {
      m_args.erase(m_args.begin() + ind);
    } else {
      ++ind;
    }
  }
  CHECK(m_args.size() == 0);

  if (m_mode != "interpolate") {
    ostringstream msg;
    msg << "combination mode unknown: " << m_mode;
    throw runtime_error(msg.str());
  }

  size_t numWeights = m_numScoreComponents;
  if (m_mode == "interpolate") {
    numWeights--;
  }
  CHECK(m_pdStr.size() == m_multimodelweights.size() || m_pdStr.size()*numWeights == m_multimodelweights.size());
}

PhraseDictionaryMultiModel::PhraseDictionaryMultiModel(const std::string &description, const std::string &line)
  :PhraseDictionary(description, line)
{
  size_t ind = 0;
  while (ind < m_args.size()) {
    vector<string> &args = m_args[ind];
    bool consumed = SetParameter(args[0], args[1]);
    if (consumed) {
      m_args.erase(m_args.begin() + ind);
    } else {
      ++ind;
    }
  }

  if (description == "PhraseDictionaryMultiModelCounts") {
    CHECK(m_pdStr.size() == m_multimodelweights.size() || m_pdStr.size()*4 == m_multimodelweights.size());
  }
}

bool PhraseDictionaryMultiModel::SetParameter(const std::string& key, const std::string& value)
{
  if (key == "mode") {
    m_mode = value;
  } else if (key == "components") {
    m_pdStr = Tokenize(value, ",");
    m_numModels = m_pdStr.size();
  } else if (key == "lambda") {
    m_multimodelweights = Tokenize<float>(value, ",");
  } else {
    return false;
  }
  return true;
}

PhraseDictionaryMultiModel::~PhraseDictionaryMultiModel()
{
}

void PhraseDictionaryMultiModel::Load()
{
  // since the top X target phrases of the final model are not the same as the top X phrases of each component model,
  // one could choose a higher value than tableLimit (or 0) here for maximal precision, at a cost of speed.


  for(size_t i = 0; i < m_numModels; ++i) {
    const string &ptName = m_pdStr[i];

    PhraseDictionary *pt = FindPhraseDictionary(ptName);
    CHECK(pt);
    m_pd.push_back(pt);
  }
}

PhraseDictionary *PhraseDictionaryMultiModel::FindPhraseDictionary(const string &ptName) const
{
  cerr << ptName << endl;
  const StaticData &staticData = StaticData::Instance();
  const std::vector<PhraseDictionary*> &pts = staticData.GetPhraseDictionaries();

  PhraseDictionary *pt = NULL;
  std::vector<PhraseDictionary*>::const_iterator iter;
  for (iter = pts.begin(); iter != pts.end(); ++iter) {
    PhraseDictionary *currPt = *iter;
    cerr << currPt->GetScoreProducerDescription() << endl;
    if (currPt->GetScoreProducerDescription() == ptName) {
      pt = currPt;
      break;
    }
  }

  return pt;
}


const TargetPhraseCollection *PhraseDictionaryMultiModel::GetTargetPhraseCollection(const Phrase& src) const
{

  std::vector<std::vector<float> > multimodelweights;

  if (m_mode == "interpolate") {
    //interpolation of phrase penalty is skipped, and fixed-value (2.718) is used instead. results will be screwed up if phrase penalty is not last feature
    size_t numWeights = m_numScoreComponents-1;
    multimodelweights = getWeights(numWeights, true);
  }

  std::map<std::string,multiModelStatistics*>* allStats = new(std::map<std::string,multiModelStatistics*>);

  CollectSufficientStatistics(src, allStats);

  TargetPhraseCollection *ret = NULL;
  if (m_mode == "interpolate") {
    ret = CreateTargetPhraseCollectionLinearInterpolation(src, allStats, multimodelweights);
  }

  ret->NthElement(m_tableLimit); // sort the phrases for pruning later
  const_cast<PhraseDictionaryMultiModel*>(this)->CacheForCleanup(ret);
  RemoveAllInMap(*allStats);
  delete allStats;

  return ret;
}


void PhraseDictionaryMultiModel::CollectSufficientStatistics(const Phrase& src, std::map<std::string,multiModelStatistics*>* allStats) const
{
  for(size_t i = 0; i < m_numModels; ++i) {
    const PhraseDictionary &pd = *m_pd[i];

    TargetPhraseCollection *ret_raw = (TargetPhraseCollection*)  pd.GetTargetPhraseCollection( src);
    if (ret_raw != NULL) {

      TargetPhraseCollection::iterator iterTargetPhrase, iterLast;
      if (m_tableLimit != 0 && ret_raw->GetSize() > m_tableLimit) {
        iterLast = ret_raw->begin() + m_tableLimit;
      } else {
        iterLast = ret_raw->end();
      }

      for (iterTargetPhrase = ret_raw->begin(); iterTargetPhrase != iterLast;  ++iterTargetPhrase) {
        TargetPhrase * targetPhrase = *iterTargetPhrase;
        std::vector<float> raw_scores = targetPhrase->GetScoreBreakdown().GetScoresForProducer(&pd);

        std::string targetString = targetPhrase->GetStringRep(m_output);
        if (allStats->find(targetString) == allStats->end()) {

          multiModelStatistics * statistics = new multiModelStatistics;
          statistics->targetPhrase = new TargetPhrase(*targetPhrase); //make a copy so that we don't overwrite the original phrase table info

          // zero out scores from original phrase table
          statistics->targetPhrase->GetScoreBreakdown().ZeroDenseFeatures(&pd);

          Scores scoreVector(m_numScoreComponents);
          statistics->p.resize(m_numScoreComponents);
          for(size_t j = 0; j < m_numScoreComponents; ++j) {
            statistics->p[j].resize(m_numModels);
            scoreVector[j] = -raw_scores[j];
          }

          statistics->targetPhrase->GetScoreBreakdown().Assign(this, scoreVector); // set scores to 0
          statistics->targetPhrase->Evaluate(src, GetFeaturesToApply());

          (*allStats)[targetString] = statistics;

        }
        multiModelStatistics * statistics = (*allStats)[targetString];

        for(size_t j = 0; j < m_numScoreComponents; ++j) {
          statistics->p[j][i] = UntransformScore(raw_scores[j]);
        }

        (*allStats)[targetString] = statistics;
      }
    }
  }
}


TargetPhraseCollection* PhraseDictionaryMultiModel::CreateTargetPhraseCollectionLinearInterpolation(const Phrase& src, std::map<std::string,multiModelStatistics*>* allStats, std::vector<std::vector<float> > &multimodelweights) const
{
  TargetPhraseCollection *ret = new TargetPhraseCollection();
  for ( std::map< std::string, multiModelStatistics*>::const_iterator iter = allStats->begin(); iter != allStats->end(); ++iter ) {

    multiModelStatistics * statistics = iter->second;

    Scores scoreVector(m_numScoreComponents);

    for(size_t i = 0; i < m_numScoreComponents-1; ++i) {
      scoreVector[i] = TransformScore(std::inner_product(statistics->p[i].begin(), statistics->p[i].end(), multimodelweights[i].begin(), 0.0));
    }

    //assuming that last value is phrase penalty
    scoreVector[m_numScoreComponents-1] = 1.0;

    statistics->targetPhrase->GetScoreBreakdown().Assign(this, scoreVector);
    statistics->targetPhrase->Evaluate(src, GetFeaturesToApply());

    ret->Add(new TargetPhrase(*statistics->targetPhrase));
  }
  return ret;
}


//TODO: is it worth caching the results as long as weights don't change?
std::vector<std::vector<float> > PhraseDictionaryMultiModel::getWeights(size_t numWeights, bool normalize) const
{
  const std::vector<float>* weights_ptr;
  std::vector<float> raw_weights;
  const StaticData &staticData = StaticData::Instance();

  weights_ptr = staticData.GetTemporaryMultiModelWeightsVector();

  // HIEU - uninitialised variable.
  //checking weights passed to mosesserver; only valid for this sentence; *don't* raise exception if client weights are malformed
  if (weights_ptr == NULL || weights_ptr->size() == 0) {
    weights_ptr = &m_multimodelweights; //fall back to weights defined in config
  } else if(weights_ptr->size() != m_numModels && weights_ptr->size() != m_numModels * numWeights) {
    //TODO: can we pass error message to client if weights are malformed?
    std::stringstream strme;
    strme << "Must have either one multimodel weight per model (" << m_numModels << "), or one per weighted feature and model (" << numWeights << "*" << m_numModels << "). You have " << weights_ptr->size() << ". Reverting to weights in config";
    UserMessage::Add(strme.str());
    weights_ptr = &m_multimodelweights; //fall back to weights defined in config
  }

  //checking weights defined in config; only valid for this sentence; raise exception if config weights are malformed
  if (weights_ptr == NULL || weights_ptr->size() == 0) {
    for (size_t i=0; i < m_numModels; i++) {
      raw_weights.push_back(1.0/m_numModels); //uniform weights created online
    }
  } else if(weights_ptr->size() != m_numModels && weights_ptr->size() != m_numModels * numWeights) {
    std::stringstream strme;
    strme << "Must have either one multimodel weight per model (" << m_numModels << "), or one per weighted feature and model (" << numWeights << "*" << m_numModels << "). You have " << weights_ptr->size() << ".";
    UTIL_THROW(util::Exception, strme.str());
  } else {
    raw_weights = *weights_ptr;
  }

  std::vector<std::vector<float> > multimodelweights (numWeights);

  for (size_t i=0; i < numWeights; i++) {
    std::vector<float> weights_onefeature (m_numModels);
    if(raw_weights.size() == m_numModels) {
      weights_onefeature = raw_weights;
    } else {
      copy ( raw_weights.begin()+i*m_numModels, raw_weights.begin()+(i+1)*m_numModels, weights_onefeature.begin() );
    }
    if(normalize) {
      multimodelweights[i] = normalizeWeights(weights_onefeature);
    } else {
      multimodelweights[i] = weights_onefeature;
    }
  }

  return multimodelweights;
}

std::vector<float> PhraseDictionaryMultiModel::normalizeWeights(std::vector<float> &weights) const
{
  std::vector<float> ret (m_numModels);
  float total = std::accumulate(weights.begin(),weights.end(),0.0);
  for (size_t i=0; i < weights.size(); i++) {
    ret[i] = weights[i]/total;
  }
  return ret;
}


ChartRuleLookupManager *PhraseDictionaryMultiModel::CreateRuleLookupManager(const InputType&, const ChartCellCollectionBase&)
{
  UTIL_THROW(util::Exception, "Phrase table used in chart decoder");
}


//copied from PhraseDictionaryCompact; free memory allocated to TargetPhraseCollection (and each TargetPhrase) at end of sentence
void PhraseDictionaryMultiModel::CacheForCleanup(TargetPhraseCollection* tpc)
{
#ifdef WITH_THREADS
  boost::mutex::scoped_lock lock(m_sentenceMutex);
  PhraseCache &ref = m_sentenceCache[boost::this_thread::get_id()];
#else
  PhraseCache &ref = m_sentenceCache;
#endif
  ref.push_back(tpc);
}


void PhraseDictionaryMultiModel::CleanUpAfterSentenceProcessing(const InputType &source)
{
#ifdef WITH_THREADS
  boost::mutex::scoped_lock lock(m_sentenceMutex);
  PhraseCache &ref = m_sentenceCache[boost::this_thread::get_id()];
#else
  PhraseCache &ref = m_sentenceCache;
#endif
  for(PhraseCache::iterator it = ref.begin(); it != ref.end(); it++) {
    delete *it;
  }

  PhraseCache temp;
  temp.swap(ref);

  CleanUpComponentModels(source);

  const StaticData &staticData = StaticData::Instance();
  std::vector<float> empty_vector;
  (const_cast<StaticData&>(staticData)).SetTemporaryMultiModelWeightsVector(empty_vector);
}


void  PhraseDictionaryMultiModel::CleanUpComponentModels(const InputType &source)
{
  for(size_t i = 0; i < m_numModels; ++i) {
    m_pd[i]->CleanUpAfterSentenceProcessing(source);
  }
}

#ifdef WITH_DLIB
vector<float> PhraseDictionaryMultiModel::MinimizePerplexity(vector<pair<string, string> > &phrase_pair_vector)
{

  const StaticData &staticData = StaticData::Instance();
  const string& factorDelimiter = staticData.GetFactorDelimiter();

  map<pair<string, string>, size_t> phrase_pair_map;

  for ( vector<pair<string, string> >::const_iterator iter = phrase_pair_vector.begin(); iter != phrase_pair_vector.end(); ++iter ) {
    phrase_pair_map[*iter] += 1;
  }

  vector<multiModelStatisticsOptimization*> optimizerStats;

  for ( map<pair<string, string>, size_t>::iterator iter = phrase_pair_map.begin(); iter != phrase_pair_map.end(); ++iter ) {

    pair<string, string> phrase_pair = iter->first;
    string source_string = phrase_pair.first;
    string target_string = phrase_pair.second;

    vector<float> fs(m_numModels);
    map<string,multiModelStatistics*>* allStats = new(map<string,multiModelStatistics*>);

    Phrase sourcePhrase(0);
    sourcePhrase.CreateFromString(Input, m_input, source_string, factorDelimiter, NULL);

    CollectSufficientStatistics(sourcePhrase, allStats); //optimization potential: only call this once per source phrase

    //phrase pair not found; leave cache empty
    if (allStats->find(target_string) == allStats->end()) {
      RemoveAllInMap(*allStats);
      delete allStats;
      continue;
    }

    multiModelStatisticsOptimization* targetStatistics = new multiModelStatisticsOptimization();
    targetStatistics->targetPhrase = new TargetPhrase(*(*allStats)[target_string]->targetPhrase);
    targetStatistics->p = (*allStats)[target_string]->p;
    targetStatistics->f = iter->second;
    optimizerStats.push_back(targetStatistics);

    RemoveAllInMap(*allStats);
    delete allStats;
  }

  Sentence sentence;
  CleanUpAfterSentenceProcessing(sentence); // free memory used by compact phrase tables

  size_t numWeights = m_numScoreComponents;
  if (m_mode == "interpolate") {
    //interpolation of phrase penalty is skipped, and fixed-value (2.718) is used instead. results will be screwed up if phrase penalty is not last feature
    numWeights = m_numScoreComponents-1;
  }

  vector<float> ret (m_numModels*numWeights);
  for (size_t iFeature=0; iFeature < numWeights; iFeature++) {

    CrossEntropy * ObjectiveFunction = new CrossEntropy(optimizerStats, this, iFeature);

    vector<float> weight_vector = Optimize(ObjectiveFunction, m_numModels);

    if (m_mode == "interpolate") {
      weight_vector = normalizeWeights(weight_vector);
    }

    cerr << "Weight vector for feature " << iFeature << ": ";
    for (size_t i=0; i < m_numModels; i++) {
      ret[(iFeature*m_numModels)+i] = weight_vector[i];
      cerr << weight_vector[i] << " ";
    }
    cerr << endl;
    delete ObjectiveFunction;
  }

  RemoveAllInColl(optimizerStats);
  return ret;

}

vector<float> PhraseDictionaryMultiModel::Optimize(OptimizationObjective *ObjectiveFunction, size_t numModels)
{

  dlib::matrix<double,0,1> starting_point;
  starting_point.set_size(numModels);
  starting_point = 1.0;

  try {
    dlib::find_min_bobyqa(*ObjectiveFunction,
                          starting_point,
                          2*numModels+1,    // number of interpolation points
                          dlib::uniform_matrix<double>(numModels,1, 1e-09),  // lower bound constraint
                          dlib::uniform_matrix<double>(numModels,1, 1e100),   // upper bound constraint
                          1.0,    // initial trust region radius
                          1e-5,  // stopping trust region radius
                          10000    // max number of objective function evaluations
                         );
  } catch (dlib::bobyqa_failure& e) {
    cerr << e.what() << endl;
  }

  vector<float> weight_vector (numModels);

  for (int i=0; i < starting_point.nr(); i++) {
    weight_vector[i] = starting_point(i);
  }

  cerr << "Cross-entropy: " << (*ObjectiveFunction)(starting_point) << endl;
  return weight_vector;
}


double CrossEntropy::operator() ( const dlib::matrix<double,0,1>& arg) const
{
  double total = 0.0;
  double n = 0.0;
  std::vector<float> weight_vector (m_model->m_numModels);

  for (int i=0; i < arg.nr(); i++) {
    weight_vector[i] = arg(i);
  }
  if (m_model->m_mode == "interpolate") {
    weight_vector = m_model->normalizeWeights(weight_vector);
  }

  for ( std::vector<multiModelStatisticsOptimization*>::const_iterator iter = m_optimizerStats.begin(); iter != m_optimizerStats.end(); ++iter ) {
    multiModelStatisticsOptimization* statistics = *iter;
    size_t f = statistics->f;

    double score;
    score = std::inner_product(statistics->p[m_iFeature].begin(), statistics->p[m_iFeature].end(), weight_vector.begin(), 0.0);

    total -= (FloorScore(TransformScore(score))/TransformScore(2))*f;
    n += f;
  }
  return total/n;
}

#endif

} //namespace