#include "ipc/candidate_ui_action_policy.h"
#include "tests/includes/test_framework.h"

TEST_CASE(candidate_ui_action_accepts_exact_one_based_page_range)
{
    REQUIRE(FanyImeIpc::IsValidCandidateUiOneBasedIndex(1));
    REQUIRE(FanyImeIpc::IsValidCandidateUiOneBasedIndex(9));
    REQUIRE(FanyImeIpc::IsValidCandidateUiOneBasedIndex(10));

    REQUIRE(!FanyImeIpc::IsValidCandidateUiOneBasedIndex(0));
    REQUIRE(!FanyImeIpc::IsValidCandidateUiOneBasedIndex(-1));
    REQUIRE(!FanyImeIpc::IsValidCandidateUiOneBasedIndex(11));
}

TEST_CASE(ai_task_results_cannot_change_the_dictionary_under_the_question_input)
{
    REQUIRE(!FanyImeIpc::SupportsCandidateDictionaryMutation(CandidateSource::AiQuestion));
    REQUIRE(!FanyImeIpc::SupportsCandidateDictionaryMutation(CandidateSource::AiCompletion));
    REQUIRE(FanyImeIpc::SupportsCandidateDictionaryMutation(CandidateSource::AiSuggestion));
    REQUIRE(FanyImeIpc::SupportsCandidateDictionaryMutation(CandidateSource::AppDictionary));
}
