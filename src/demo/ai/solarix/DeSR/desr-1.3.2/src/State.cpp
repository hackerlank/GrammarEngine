/*
**  DeSR
**  src/State.cpp
**  ----------------------------------------------------------------------
**  Copyright (c) 2008  Giuseppe Attardi (attardi@di.unipi.it).
**  ----------------------------------------------------------------------
**
**  This file is part of DeSR.
**
**  DeSR is free software; you can redistribute it and/or modify it
**  under the terms of the GNU General Public License, version 3,
**  as published by the Free Software Foundation.
**
**  DeSR is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with this program.  If not, see <http://www.gnu.org/licenses/>.
**  ----------------------------------------------------------------------
*/

#include "State.h"
#include "conf_feature.h"
#include "version.h"

// standard
#include <assert.h>
//#include <alloca.h>
#include <set>

// IXE library
#include "text/WordSet.h"
#include "conf/conf_float.h"

using namespace std;
using namespace IXE;
using namespace Tanl::Text;
using namespace Tanl::Classifier;

// Tanl
using Tanl::Language;
using Tanl::Sentence;
using Tanl::Token;
using Tanl::TreeToken;

namespace Parser {

float const	GlobalInfo::freqRatio = 1.5;

FeatureConfig::FeatureConfig(IXE::Configuration::Params& params) :
  IXE::Configuration(params),

  features	("Feature", params),
  singleFeatures("Features", &*features, params),
  arcEager	("ArcEager", false, params),
  SplitFeature	("SplitFeature", params),
  ClosestChildren	("ClosestChildren", false, params),
  PrepChildEntityType	("PrepChildEntityType", false, params),
  StackSize	("StackSize", true, params),
  InputSize	("InputSize", false, params),
  InPunct	("InPunct", false, params),
  InQuotes	("InQuotes", false, params),
  VerbCount	("VerbCount", true, params),
  UseChildPunct	("UseChildPunct", true, params),
  PastActions	("PastActions", 1, params),
  WordDistance	("WordDistance", true, params),
  PunctCount	("PunctCount", true, params),
  MorphoAgreement	("MorphoAgreement", false, params),
  LexChildNonWord	("LexChildNonWord", true, params),
  SingleRoot	("SingleRoot", true, params),
  CompositeActions	("CompositeActions", true, params),
  SecondOrder	("SecondOrder", false, params),
  RightToLeft	("RightToLeft", false, params),
  unambiguous	("UnambiguousFeatures", true, params),

  fileVersion	("Version", version, params),
  lexCutoff	("LexCutoff", 0, params)
{ }

WordSet	actionTable;

// pattern for detecting punctuation:
RegExp::Pattern State::ispunct("^\\p{P}+$",
			       PCRE_UTF8 | PCRE_NO_UTF8_CHECK);
// pattern for detecting quotes:
RegExp::Pattern State::isOpenQuote("^\\p{Pi}$", PCRE_UTF8 | PCRE_NO_UTF8_CHECK);
RegExp::Pattern State::isCloseQuote("^\\p{Pf}$", PCRE_UTF8 | PCRE_NO_UTF8_CHECK);
RegExp::Pattern State::nonWordAscii("^[^$0-9_-zA-Z]+$");

char const* MakeAction(bool CompositeActions, char const* a, string const& dep)
{
  if (CompositeActions || !strcmp(a, "D")) {
    char action[128];
    sprintf(action, "%s%s", a, dep.c_str());
    return *actionTable.insert(action).first;
  } else
    return *actionTable.insert(a).first;
}
#define mkAction(a, dep) MakeAction(CompositeActions, a, dep)

char const* actionString(char const* a)
{
    return *actionTable.insert(a).first;
}

//======================================================================
// SentenceInfo

SentenceInfo::SentenceInfo(Sentence& sentence, GlobalInfo* info) :
  globalInfo(info)
{
  if (sentence.empty())
    return;
  // count punctuations
  bool inquote = false;
  for (unsigned i = 0; i < sentence.size(); ++i) {
    Token* token = sentence[i]->token;
    string& form = token->form;
    if (i == 0)
      punctCount.push_back(State::ispunct.test(form));
    else
      punctCount.push_back(punctCount[i-1] + State::ispunct.test(form));
    if (State::isOpenQuote.test(form) || !inquote && form == "\"") {
      inQuotes.push_back(inquote);
      inquote = true;
    } else if (State::isCloseQuote.test(form) || inquote && form == "\"") {
      inquote = false;
      inQuotes.push_back(inquote);
    } else
      inQuotes.push_back(inquote);
  }
}

//======================================================================
// State

State::State(Sentence const& sent, GlobalInfo* info) :
  sentence(sent),		// private copy
  rootNode(new TreeToken(0, "#NULL")),
  action(0),
  previous(0),
  afterUnshift(false)
{
  if (info->config->RightToLeft)
    sentence.reverse();
  sentenceInfo = new SentenceInfo(sentence, info);
  // initialize input
  input.resize(sentence.size());
  std::copy(sentence.rbegin(), sentence.rend(), input.begin());
  // initialize stack
  stack.push_back(rootNode);
}

bool State::hasNext()
{
  return !input.empty();
}

/**
 * Parsing actions
 */
inline State* State::Shift()
{
  TreeToken* next = input.back();
  input.pop_back();
  stack.push_back(next);
  action = "S";		// makes history
  afterUnshift = false;
  return this;
}

inline State* State::Unshift()
{
  if (stack.size() < 2)
    return 0;
  TreeToken* top = stack.back();
  stack.pop_back();
  input.push_back(top);
  action = "U";		// makes history
  afterUnshift = true;
  return this;
}

inline State* State::Right(Action action)
{
  // pop top and add it as left child to next token
  if (stack.size() == 1)
    return 0;
  TreeToken* top = copy(stack.back());
  stack.pop_back();
  TreeToken* next = copy(input.back());
  input.back() = next;
  next->left.push_back(top);
  top->linkHead(next->id, 0);
  if (sentenceInfo->globalInfo->config->CompositeActions)
    top->linkLabel(action+1);
  this->action = actionString(action);	// get unique copy
  return this;
}

inline State* State::Left(Action action)
{
  FeatureConfig* config = sentenceInfo->globalInfo->config;

  // pop top, add to it next token as right child and replace next
  TreeToken* top = copy(stack.back());
  TreeToken* next = copy(input.back());
  top->right.push_back(next);
  if (config->arcEager) {
    // Shift
    stack.push_back(next);
    input.pop_back();
  } else if (config->CompositeActions) {
    if (stack.size() > 1) {
      stack.pop_back();
      input.back() = top;
    } else {
      // optimize, anticipating Shift
      stack.back() = top;
      input.pop_back();
    }
  } else {
    if (stack.size()) {
      stack.pop_back();
      input.back() = top;
    }
  }
  next->linkHead(top->id, 0);
  if (config->CompositeActions)
    next->linkLabel(action+1);
  this->action = actionString(action);	// get unique copy
  return this;
}

inline State* State::right(Action action)
{
  unsigned n = action[1] - '0';
  // pop n-th top and add it as left child to next token
  if (stack.size() <= n) {	// stack n
    // Don't extract DummyRoot
    //cerr << "Improper action " << action << endl;
    return 0;
  }
  TreeToken* nthTop = copy(stack[stack.size() - n]);
  stack.erase(stack.end() - n);
  TreeToken* next = copy(input.back());
  next->left.push_back(nthTop);
  nthTop->linkHead(next->id, 0);
  if (sentenceInfo->globalInfo->config->CompositeActions) {
    nthTop->linkLabel(action+2);
    // move back
    input.push_back(stack.back());
    stack.pop_back();
  }
  this->action = actionString(action);	// get unique copy
  return this;
}

inline State* State::left(char const* action)
{			// l2, l3, l4
  unsigned n = action[1] - '0';
  // add next token as right child to n-th top,
  // move n tokens from stack to input
  if (stack.size() < n) {
    //cerr << "Improper action " << action << endl;
    return 0;
  }
  TreeToken* nthTop = copy(stack[stack.size() - n]);
  TreeToken* next = copy(input.back());
  nthTop->right.push_back(next);
  next->linkHead(nthTop->id, 0);
  if (sentenceInfo->globalInfo->config->CompositeActions)
    next->linkLabel(action+2);
  // move first token
  input.back() = stack.back();
  stack.pop_back();
  // move n-2 tokens back to input
  for (unsigned i = 0; i < n-2; i++) {
    input.push_back(stack.back());
    stack.pop_back();
  }
  if (stack.size() > 1) {	// avoid popping ROOT
    // move nth token back to input
    input.push_back(nthTop);
    stack.pop_back();
  } else {
    // anticipate Shift()
    stack.back() = nthTop;
  }
  this->action = actionString(action);	// get unique copy
  return this;
}

inline State* State::DepLink(Action action)
{
  TreeToken* next = input.back();
  switch (previous ? previous->action[0] : this->action[0]) { // TrainState has no previous
  case 'R':
  case 'r':
    // add dependency link to the leftmost child of next
    next->left.back()->linkLabel(action+1);
    // if previous action was an r_i,
    // complete previous action by moving back one token to input
    if (this->action[0] == 'r') {
      input.push_back(stack.back());
      stack.pop_back();
    }
    this->action = actionString(action);	// get unique copy
    return this;

  case 'L':
  case 'l':
    // add dependency link to the rightmost child of next
    next->right.back()->linkLabel(action+1);
    // if stack is empty complete previous action by doing a Shift
    if (stack.empty()) {	// link to rootNode, restore it
      input.pop_back();
      stack.push_back(next);
    }
    this->action = actionString(action);	// get unique copy
    return this;
  }
  return this;
}

State* State::Extract()
{
  // move second stack token to Extracted and Shift
  if (stack.size() < 3 ||	// stack (2 + root)
      input.size() < 1) {	// impossible to extract
    //cerr << "Improper action " << action << endl;
    return 0;
  }
  TreeToken* nthStack = stack[stack.size() - 2];
  extracted.push_back(nthStack);
  stack.erase(stack.end() - 2);
  // Shift
  TreeToken* next = input.back();
  stack.push_back(next);
  input.pop_back();
  action = "E";
  return this;
}

State* State::Insert()
{
  // move token from Extracted to next
  if (extracted.empty()) {
    //cerr << "Improper action " << action << endl;
    return 0;
  }
  input.push_back(extracted.back());
  extracted.pop_back();
  action = "I";
  return this;
}

State* State::Pop()
{
  if (stack.size() < 2)
    return 0;
  stack.pop_back();
  action = "P";
  return this;
}

/**
 *	Perform a parsing action. Actions can be:
 *	Left, Right, Shift, Left2, Right2, Left3, Right3, Left4, Right4,
 *	Extract, Insert (the last two are obsolete).
 *	Left and Right operate on top of stack and next sentence token.
 *	Left2 and Right2 operate on second top of stack and next sentence token.
 *	Left3 and Right3 operate on third top of stack and next sentence token.
 *	Left4 and Right4 operate on fourth top of stack and next sentence token.
 *	Extract and Insert, move/restore token to/from extracted.
 *
 *	The reduce actions (Left/Right) are combined with the deprel to be
 *	assigned to the link created.
 *	If CompositeAction is false, instead, there are separate additional
 *	actions DepLeft and DepRight, used to assign the label to a link
 *	created with the preceding reduce action. DepLeft and DepRight are
 *	paired with the dependency label to be assigned.
 *
 *	@return the new State after the transition.
 */
State* State::transition(Action action)
{
# ifdef DEBUG_1
  showStatus();
  cerr << "Action: " << action << endl;
# endif
  switch (action[0]) {
  case 'S':
    if (input.empty())
      return this;		// extra dummy Shift at end of sequence
    return Shift();
  case 'R':
    if (stack.size() == 1) {	// stack root
      // Don't extract DummyRoot
      // Force a Shift
      return Shift();
    }
    return Right(action);
  case 'L':
    return Left(action);
  case 'r':			// r2, r3, r4
    return right(action);
  case 'l':			// l2, l3, l4
    return left(action);
  case 'D':			// DepLink
    return DepLink(action);
  case 'E':
    return Extract();
  case 'I':
    return Insert();
  case 'P':
    return Pop();
  case 'U':
    return Unshift();
  }
  return 0;
}

// action is supplied only during training
void State::predicates(Features& preds, Action action)
{
  FeatureConfig* config = sentenceInfo->globalInfo->config;

  preds.clear();
  // special case: it helps learning to do a Shift
  if (stack.empty()) {     // happens only after Left action to rootNode
    preds.push_back("(");
    if (config->CompositeActions)
      return;
  }
  // may be redundant
  if (input.empty()) {
    preds.push_back(")");
    return;
  }

  // Token features
  tokenFeatures(preds);

  // Features from Extracted stack.
  char feature[4096];
  if (extracted.size()) {
    Token* tok = extracted.back()->token;
    string const* lemma = tok->lemma();
    if (lemma && !lemma->empty()) {
      snprintf(feature, sizeof(feature), "EL%s", lemma->c_str());
      preds.push_back(feature);
    } else {
      snprintf(feature, sizeof(feature), "EW%s", tok->form.c_str());
      preds.push_back(feature);
    }
    string const* pos = tok->pos();
    if (pos && !pos->empty()) {
      snprintf(feature, sizeof(feature), "EP%s", pos->c_str());
      preds.push_back(feature);
    }
  }

  Language const* lang = sentence.language;
  // Morpho agreement
  if (config->MorphoAgreement && stack.size() > 1) {
    Token* top = stack.back()->token;
    Token* next = input.back()->token;
    if (!lang->morphoLeft(*top->pos()) &&
	!lang->morphoRight(*next->pos())) {
      if (top->morpho.number &&
	  !lang->numbAgree(top->morpho.number, next->morpho.number))
	preds.push_back("!=N");
      if (top->morpho.gender &&
	  !lang->gendAgree(top->morpho.gender, next->morpho.gender))
	preds.push_back("!=G");
      /* FIXME: This does not solve: "la caserma dei carabinieri piu' vicina"
	 and decreases LAS. */
      if (next->morpho.number && next->morpho.gender &&
	  lang->numbAgree(top->morpho.number, next->morpho.number) &&
	  lang->gendAgree(top->morpho.gender, next->morpho.gender)) {
	if (input.size() > 1) {
	  Token* ahead = input[input.size()-2]->token;
	  if (ahead->morpho.number && ahead->morpho.gender &&
	      !lang->morphoRight(*ahead->pos()) &&
	      (!lang->numbAgree(next->morpho.number, ahead->morpho.number) ||
	       !lang->gendAgree(next->morpho.gender, ahead->morpho.gender)))
	    preds.push_back("=NG!1");

	  if (input.size() > 2) {
	    ahead = input[input.size()-3]->token;
	    if (ahead->morpho.number && ahead->morpho.gender &&
		!lang->morphoRight(*ahead->pos()) &&
		(!lang->numbAgree(next->morpho.number, ahead->morpho.number) ||
		 !lang->gendAgree(next->morpho.gender, ahead->morpho.gender)))
	      preds.push_back("=NG!2");
	  }
	}
      }
    }
  }

  // Sentence context predicates
  if (config->StackSize && stack.size() > 2) // stack 1
    preds.push_back("((");
  if (config->InputSize && input.size() > 1)
    preds.push_back("))");
  if (config->VerbCount) {
    int vc = 0;
    for (size_t i = 1; i < stack.size(); i++) // skip rootNode
      if (stack[i]->token->isVerb(lang))
	vc++;
    if (vc) {
      snprintf(feature, sizeof(feature), "VC%d", vc);
      preds.push_back(feature);
    }
  }

  // Punctuation presence
  int id = input.back()->id;
  if (id > 1) {
    // Punctuation balance (odd/even count)
    if (config->InPunct && sentenceInfo->punctCount[id-2]%2)
      preds.push_back(".");
    // Punctuation presence
    if (config->PunctCount && sentenceInfo->punctCount[id-2]) {
      snprintf(feature, sizeof(feature), ".%d", sentenceInfo->punctCount[id-2]);
      preds.push_back(feature);
    }
  }
  // Within quotes
  if (config->InQuotes && sentenceInfo->inQuotes[id-1]) {
    snprintf(feature, sizeof(feature), "0\"");
    preds.push_back(feature);
  }

  if (config->UseChildPunct) {
    // notice if there is a punctuation among children of top
    // Useful to handle properly phrases like:
    // fabricante de " software "
    if (stack.size() > 1) {
      TreeToken* top = stack.back();
      FOR_EACH (vector<TreeToken*>, top->left, it) {
	if (ispunct.test((*it)->token->form)) {
	  snprintf(feature, sizeof(feature), "1.<%s", (*it)->token->form.c_str());
	  preds.push_back(feature);
	  break;
	}
      }
      for (vector<TreeToken*>::reverse_iterator it = top->right.rbegin();
	   it != top->right.rend(); it++) {
	if (ispunct.test((*it)->token->form)) {
	  snprintf(feature, sizeof(feature), "1.>%s", (*it)->token->form.c_str());
	  preds.push_back(feature);
	  break;
	}
      }
    }
    if (input.size()) {
      TreeToken* next = input.back();
      // notice if there is a punctuation among children of next
      FOR_EACH (vector<TreeToken*>, next->left, it) {
	if (ispunct.test((*it)->token->form)) {
	  snprintf(feature, sizeof(feature), ".<0%s", (*it)->token->form.c_str());
	  preds.push_back(feature);
	  break;
	}
      }
      for (vector<TreeToken*>::reverse_iterator it = next->right.rbegin();
	   it != next->right.rend(); it++) {
	if (ispunct.test((*it)->token->form)) {
	  snprintf(feature, sizeof(feature), ".>0%s", (*it)->token->form.c_str());
	  preds.push_back(feature);
	  break;
	}
      }
    }
  }
  bool oldVersion = *config->fileVersion == "1.1.2";
  // History features
  State const* s = this;
  for (int i = 0; i < config->PastActions && s; i++, s = s->previous) {
    if (s->action) {
      if (oldVersion)
	snprintf(feature, sizeof(feature), "A%d%s", i, s->action);
      else
	snprintf(feature, sizeof(feature), "a%d%s", i, s->action);
      preds.push_back(feature);
    }
  }
  // Focus word distance
  if (config->WordDistance && stack.size()) {
    int d = abs((int)input.back()->id - (int)stack.back()->id) - 1;
    snprintf(feature, sizeof(feature), "%d", min(d, 4));
    preds.push_back(feature);
  }

  // Global corpus features
  // add entity type (time/location) of children of prepositions
  if (config->PrepChildEntityType)
    prepChildEntities(preds);

  if (config->SecondOrder) {
    // add all pairs
    size_t predNo = preds.size();
    for (unsigned i = 0; i < predNo; i++) {
      for (unsigned j = i+1; j < predNo; j++) {
	// combine in alphabetical order
	string combo = (preds[i].compare(preds[j]) < 0) ?
	  preds[i] + '#' + preds[j] : preds[j] + '#' + preds[i];
	preds.push_back(combo.c_str());
      }
    }
  }
  // Features for predicting DEPREL
  if (!config->CompositeActions && this->action) { // not initial state
    // add pair with POS of tokens to be linked
    switch (this->action[0]) {	// previous action
    case 'R':
    case 'r': {
      TreeToken* next = input.back();
      string const* npos = next->token->pos();
      string const* nlpos = next->left.back()->token->pos();
      if (npos && nlpos) {
	snprintf(feature, sizeof(feature), "d%s%s", nlpos->c_str(), npos->c_str());
	preds.push_back(feature);
      }
      break;
    }
    case 'L':
    case 'l': {
      TreeToken* next = input.back();
      string const* npos = next->token->pos();
      string const* nrpos = next->right.back()->token->pos();
      if (npos && nrpos) {
	snprintf(feature, sizeof(feature), "D%s%s", nrpos->c_str(), npos->c_str());
	preds.push_back(feature);
      }
      break;
    }
    }
  }
}

static void childPunctFeature(Features& preds, TreeToken* tok, int root,
			      set<TreeToken*>& lexChildNonWordTokens,
			      bool unambiguous)
{
  char feature[8];
  if (lexChildNonWordTokens.find(tok) == lexChildNonWordTokens.end()) {
    // notice if there are puctuation or non ASCII word characters in children
    lexChildNonWordTokens.insert(tok); // add to list to avoid repeating for same token on different features
    FOR_EACH (vector<TreeToken*>, tok->left, it) {
      if (State::nonWordAscii.test((*it)->token->form)) {
	if (unambiguous)
	  snprintf(feature, sizeof(feature), "/.%d", root);
	else
	  snprintf(feature, sizeof(feature), ".%d/", root);
	preds.push_back(feature);
	break;
      }
    }
    for (vector<TreeToken*>::reverse_iterator it = tok->right.rbegin();
	 it != tok->right.rend(); it++) {
      if (State::nonWordAscii.test((*it)->token->form)) {
	if (unambiguous)
	  snprintf(feature, sizeof(feature), "\\.%d", root);
	else
	  snprintf(feature, sizeof(feature), ".%d\\", root);
	preds.push_back(feature);
	break;
      }
    }
  }
}

// positions on the stack are numbered -1, -2, ...
// positions on input are numbered 0, 1, 2, ...
void State::tokenFeatures(Features& preds)
{
  char feature[4096];
  char* end = feature + sizeof(feature);
  Token* next = input.back()->token;
  set<TreeToken*> lexChildNonWordTokens;

  FeatureConfig* config = sentenceInfo->globalInfo->config;

  bool oldVersion = *config->fileVersion == "1.1.2";

  FOR_EACH (FeatureSpecs, *config->features, fit) {
    char* fill = feature;
    for (FeatureSpec* fs = *fit; fs; fs = fs->next) {
      char const* attrName = fs->attribute;
      int attrIndex =
	oldVersion ?
	next->attrIndex(attrName) :
	conf_features::featureIndex[attrName];
      char featId = 'A' + attrIndex; // feature type identifier
      // find token
      TokenPath& tp = *fs->path;
      TreeToken* tok;
      if (tp.root < 0) {
	if (-tp.root > (int)stack.size() - 1) // -1 because of root node
	  break;
	tok = stack[stack.size() + tp.root]; // no -1 because numbered from -1
      } else {
	if (tp.root >= (int)input.size())
	  break;
	tok = input[input.size() - 1 - tp.root];
      }
      tok = tok->follow(tp, sentence);
      if (tok) {
	string const* item = tok->predicted(attrName);
	if (!item || item->empty()) {
	  // skip empty attributes
	  break;
	}
	if (config->unambiguous) {
	  // put the path in front
	  if (tp.root < 0)
	    fill += snprintf(fill, end - fill, "%s%d%c%s", tp.Code(), -tp.root, featId, item->c_str());
	  else
	    fill += snprintf(fill, end - fill, "%s%c%d%s", tp.Code(), featId, tp.root, item->c_str());
	} else {
	  if (tp.root < 0)
	    fill += snprintf(fill, end - fill, "%d%c%s%s", -tp.root, featId, tp.Code(), item->c_str());
	  else
	    fill += snprintf(fill, end - fill, "%c%d%s%s", featId, tp.root, tp.Code(), item->c_str());
	}

	if (!fs->next) {
	  preds.push_back(feature);
	  if (fs == *fit && !tp.length()) // single feature, empty path
	    if (config->LexChildNonWord)
	      childPunctFeature(preds, tok, tp.root, lexChildNonWordTokens,
				config->unambiguous);
	}
      }
    }
  }

  // compute the split feature, for use in choosing among multiple SVMs.
  if (!config->SplitFeature->empty()) {
    FeatureSpec* split = (*config->SplitFeature)[0];
    char const* attrName = split->attribute;
    // find token
    TokenPath& tp = *split->path;
    TreeToken* tok = 0;
    if (tp.root < 0 &&
	-tp.root <= (int)stack.size() - 1) // -1 because of root node
      tok = stack[stack.size() + tp.root]; // no -1 because numbered from -1
    else if (tp.root >= 0 &&
	     tp.root < (int)input.size())
      tok = input[input.size() - 1 - tp.root];
    if (tok)
      tok = tok->follow(tp, sentence);
    if (tok) {
      string const* feat = tok->predicted(attrName);
      if (feat)
	splitFeature = *feat; // do not trim feat->substr(0, 2);
      else			// FIXME: throw Error
	cerr << "Missing split feature" << endl;
    }
  }
}

void addComplementFeature(Token* tok, Language const* lang,
			  GlobalInfo* info, Features& preds,
			  char const* timePred, char const* locPred)
{
  if (tok->isNoun(lang)) {
    string const* noun = tok->lemma();
    if (noun && !noun->empty()) {
      int tc = info->timeLemmas.count(*noun);
      int lc = info->locLemmas.count(*noun);
      // 'tarda' appears in both categories
      if (tc > info->freqRatio * lc)
	preds.push_back(timePred);
      if (lc > info->freqRatio * tc)
	preds.push_back(locPred);
    }
  }
} 

/** Add features corresponding to entity type of child of preposition */
void State::prepChildEntities(Features& preds)
{
  // FIXME: should deal also with non-projective actions (r2, l2 etc.)
  Language const* lang = sentence.language;
  GlobalInfo* info = sentenceInfo->globalInfo;
  if (stack.size() > 1) {
    TreeToken* top = stack.back();
    addComplementFeature(top->token, lang, info, preds, "1TIME", "1LOC");
  }
  // same with next
  TreeToken* next = input.back();
  addComplementFeature(next->token, lang, info, preds, "TIME0", "LOC0");
}

void State::showStatus() {
  cerr << "Stack:" << endl;
  FOR_EACH (vector<TreeToken*>, stack, it)
    (*it)->print(cerr);
  cerr << "Next:" << endl;
  if (input.size())
    input.back()->print(cerr);
}

// ======================================================================
// TrainState

TrainState::TrainState(Sentence const& sent, GlobalInfo* info) :
  State(sent, info),
  annotated(sentence)		// reversed if RighToLeft
{
  // sentence is our working copy: it is a shallow copy of sent;
  // annotated is copy of original with link information.
  // count dependents for each node (used to determine when arc can be created)
  size_t len = sentence.size();
  dependents.resize(len);
  FOR_EACH (Sentence, sentence, sit) {
    int head = (*sit)->linkHead();
    if (head)
      dependents[head-1]++;
  }
  // build tree
  FOR_EACH (Sentence, annotated, sit) {
    int head = (*sit)->linkHead();
    if (head && head < (*sit)->id)
      annotated[head-1]->right.push_back(annotated[(*sit)->id-1]);
  }
  for (int id = annotated.size(); id; id--) {
    int head = annotated[id-1]->linkHead();
    if (head && id < head)
	annotated[head-1]->left.push_back(annotated[id-1]);
  }
  // add global info from sentence
  if (info->config->PrepChildEntityType)
    info->extract(sentence);
  // clear all dependencies.
  // Even during training, we should only see dependencies
  // that have been created during parsing.
  FOR_EACH (vector<TreeToken*>, input, sit) {
    (*sit)->linkHead(0);
    (*sit)->linkLabel("");
  }
  if (info->config->lexCutoff > 0)
    info->config->unambiguous = true;	// necessary to exploit cutoff during parsing
}

#define ORIG(tok) annotated[(tok)->id-1]

bool commonAncestor(TreeToken* tok, TreeToken* root, Sentence& annotated)
{
  int rootId = ORIG(root)->id;
  for (int tokId = tok->id; tokId;
       tokId = annotated[tokId - 1]->linkHead()) {
    if (rootId == tokId)
      return true;
  }
  return false;
}

// arc: next -> top[-n]
#define NextToStackLink(n) \
  (stack.size() > (n) && \
   ORIG(stack[stack.size()-(n)])->linkHead() == next->id)

// arc: top[-n] -> next
#define StackToNextLink(n) \
  (stack.size() > (n) && \
   stack[stack.size()-(n)]->id == nextHead)

#define Resolved(tok) (!dependents[(tok)->id-1])
#define hasRightChild(tok) (tok)->right.size()

#define top2 stack[stack.size()-2]
#define top3 stack[stack.size()-3]
#define top4 stack[stack.size()-4]

#define rightResolved(tok) ((tok)->right.size() == ORIG(tok)->right.size())

/*
 * Handling non projectivity.
 *
 * Consider a link wi <-> wj which crosses a link between topn (on the stack)
 * and next (on the input), where wj is outside [topn, next]:
 *
 * 	topn ... wi ... next
 *
 * There are two cases:
 *
 * 1. topn <- next
 * 	Resolved(topn) must be true for an action l2 or li to be performed
 * 2. topn -> next
 * 	Resolved(next) must be true for an action r2 or ri to be performed
 *
 * In the normal parser, all wi would have been reduced while scanning up to
 * next.
 * Therefore the presence of the link is sufficient to guarantee that the
 * action must be performed.
 *
 * In case of delayReduce, some nodes might have not been reduced, hence an
 * Unshift must be done before attempting an li/ri action.
 * Therefore the rule to apply is:
 *
 * 	if (!crossing(topn, next))
 * 	then Unshift
 * 	else perform li/ri
 *
 * crossing(i, j) is used to test whethere there is a token between i and j
 * that crosses the link between i and j.
 */

/**
 *	Determines the action (LRSEI) required to build the dependency tree.
 *	In case of ArcEager, also generates P.
 *	In case of !CompositeAction, also generates D.
 *
 *	Action E has currently been disabled.
 */
Action TrainState::nextAction()
{
  FeatureConfig* config = sentenceInfo->globalInfo->config;
  bool CompositeActions = config->CompositeActions;

  if (input.size() == 0) {
    if (stack.size() > 1)
      return "U";
    else
      return 0;
  }
  if (!CompositeActions && this->action && strchr("RrLl", this->action[0])) {
    TreeToken* next = input.back();
    switch (this->action[0]) {	// previous action
    case 'R':
    case 'r':
      return mkAction("D", *next->left.back()->get("DEPREL"));
    case 'L':
    case 'l':
      return mkAction("D", *next->right.back()->get("DEPREL"));
    }
    return 0;			// shouldn't happen
  }
  if (stack.size() == 0)
    // Empty shouldn't happen, because of dummy root.
    return "S";
  TreeToken* next = input.back();
  int nextHead = ORIG(next)->linkHead();
  string const& nextLabel = ORIG(next)->linkLabel();
  TreeToken* top = stack.back();

  if (extracted.size() && nextHead == extracted.back()->id) {
    // bring back last extracted
    // action 'I'
    return "I";
  } else if (top->id && ORIG(top)->linkHead() == next->id) {
    // right move: top => next (arc: top <- next)

    if (!Resolved(top)) {	// top has still unresolved dependents
      return "S";
    }

    // action 'R'
    dependents[next->id - 1]--;
    return mkAction("R", ORIG(top)->linkLabel());
  } else if (config->arcEager && stack.size() > 1 && Resolved(top)) {
    return "P";
  } else if (nextHead == top->id && Resolved(next)) {
    // left move: top <= next (arc: top -> next)
    // action 'L'

    // pop top and replace next (if !arcEager)
    if (stack.size() > 1) {	// except on rootNode
      dependents[top->id - 1]--;
    }
    return mkAction("L", nextLabel);
  } else if (NextToStackLink(2) && Resolved(top2)) {
    // non projective link: top2 <- next
    // action 'r2'

    dependents[next->id - 1]--;
    return mkAction("r2", ORIG(top2)->linkLabel());
  } else if (NextToStackLink(3) && Resolved(top3)) {
    // right move: top3 => next (arc: top3 <- next)
    // action 'r3'

    dependents[next->id - 1]--;
    return mkAction("r3", ORIG(top3)->linkLabel());
  } else if (input.size() == 1 && // delay as much as possible
	     // FIXME: find better heuristics
	     NextToStackLink(4) && Resolved(top4)) {
    // right move: top4 => next (arc: top4 <- next)
    // action 'r4'

    dependents[next->id - 1]--;
    return mkAction("r4", ORIG(top4)->linkLabel());
  } else if (nextHead == top->id && !Resolved(next)) {
    // arc: top -> next, but next has unresolved dependencies

    return "S";
  } else if (StackToNextLink(2) && Resolved(next)) {
    // left move: top2 <= next (arc: top2 -> next)
    // action 'l2'
    // move up to 2 tokens from stack to input, i.e. go back
    // move top token

    if (stack.size() == 2) {
      // Special case: non-projective link to root.
      // This may happens when there are multiple roots, sometimes
      // because of annotations errors like in line 25640 of
      // danish_ddt_train.conll.
      // Don't pop rootNode, so do nothing.
    } else {
      dependents[top2->id - 1]--;
      // move second token
    }
    return mkAction("l2", nextLabel);
  } else if (StackToNextLink(3) && Resolved(next)) {
    // left move: top3 <= next (arc: top3 -> next)
    // action 'l3'

    if (stack.size() > 3)
      dependents[top3->id - 1]--;
    return mkAction("l3", nextLabel);
  } else if (StackToNextLink(4) && Resolved(next)) {
    // left move: top4 <= next (arc: top4 -> next)
    // action 'l4'
    // Ex: presenza di una macchina insolita in [presenza] pianura , il gatto [macchina]

    if (stack.size() > 4)
      dependents[top4->id - 1]--;
    return mkAction("l4", nextLabel);
  }
  return "S";
}

Event* TrainState::next()
{
  Action action = nextAction();
  Event* ev = new Event(action);
  predicates(ev->features, action);
  return ev;
}

// ======================================================================
// ParseState

ParseState::ParseState(Sentence& sent, GlobalInfo* globalInfo, WordIndex& predIndex) :
  State(sent, globalInfo),
  predIndex(predIndex),
  lprob(0),
  refCount(0)
{
  // clear all dependencies.
  FOR_EACH (vector<TreeToken*>, sentence, sit) {
    (*sit)->linkHead(0);
    (*sit)->linkLabel("");
  }
}

ParseState::~ParseState() {
  assert(refCount == 0);
  if (previous) {
    ((ParseState*)previous)->decRef();
    for (size_t i = 0; i < sentence.size(); i++) {
      if (sentence[i] == previous->sentence[i])
	sentence[i] = 0;	// will be deleted by previous
    }
  }
}

/**
 *	Free the branch of derivation that lead to this state.
 */
void ParseState::prune()
{
  if (refCount == 0) {
    ParseState* p = (ParseState*)previous;
    delete this;
    if (p)
      p->prune();
  }
}

bool ParseState::hasNext()
{
  bool res = State::hasNext();
  if (!res) {
    // sometimes there are more than one root nodes
    if (stack.size() > 2) {
      // connect nodes to root
#     ifdef DEBUG
      cerr << "Multiple roots: " << stack.size() << endl;
#     endif
      Language const* lang = sentence.language;
      // find root
      int root = 0;
      int rootSize = 0;		// size of subtree
      FOR_EACH (vector<TreeToken*>, stack, sit) {
	TreeToken* node = *sit;
	if (node->linkHead() == 0) {
	  int size = node->size();
	  // FIXME: use better heuristics
	  string const* tokPos = node->token->pos();
	  if (size > rootSize && tokPos && lang->rootPos(*tokPos)) {
	    root = node->id;
	    rootSize = size;
	  }
	}
      }
      if (root) {
	// set label to root if missing
	TreeToken* rootNode = sentence[root-1];
	if (rootNode->linkLabel().empty())
	  rootNode->linkLabel(lang->rootLabel());
	TO_EACH (vector<TreeToken*>, stack, sit) {
	  TreeToken* node = *sit;
	  if (node->linkHead() == 0 && node->id != root) {
	    if (sentenceInfo->globalInfo->config->SingleRoot) {
	      node->linkHead(root);
	      if (node->linkLabel().empty())
		node->linkLabel(lang->rootLabel());
	    } else {
	      // just set dependency label (as Stanford Dependencies)
	      node->linkLabel(lang->rootLabel());
	    }
	  }
	}
      }
    }
  }
  return res;
}

Tanl::Classifier::Context* ParseState::next()
{
  Features preds;
  predicates(preds);		// get contextual features
  // convert them to PIDs
  context.clear();
  FOR_EACH (Features, preds, it) {
    string const& pred = *it;
    if (predIndex.find(pred.c_str()) != predIndex.end()) {
#     ifdef SHOW_FEATS
      cerr << pred << ' ';
#     endif
      context.add(predIndex[pred.c_str()]);
    } else {
      // try with #UNKNOWN
      size_t pathLen = strspn(pred.c_str(), TokenPath::dirCode);
      // FIXME: assumes token position is single digit.
      if (pathLen + 2 < pred.size()) {
	string uf = pred.substr(0, pathLen+2) + "#UNKNOWN";
	if (predIndex.find(uf.c_str()) != predIndex.end()) {
#         ifdef SHOW_FEATS
	  cerr << uf << ' ';
#         endif
	  context.add(predIndex[uf.c_str()]);
	}
      }
    }
  }
# ifdef SHOW_FEATS
  cerr << endl;
# endif
  return &context;
}

ParseState* ParseState::transition(Action action)
{
  // don't allow extracted token to survive beyond punctuation
  if (extracted.size() && input.size() &&
      (action[0] == 'S' || action[0] == 'L') &&
      ispunct.test(input.back()->token->form))
    action = "I";
  ParseState* next = new ParseState(*this);
  if (next->State::transition(action))
    return next;
  delete next;			// not prune(), this must survive
  return 0;
}

} // namespace Parser
