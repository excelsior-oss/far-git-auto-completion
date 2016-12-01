#include "Logic.hpp"

#include <cassert>
#include <algorithm>
#include <functional>
#include <vector>

#include "GitAutocomplete.hpp"
#include "Utils.hpp"
#include "Trie.hpp"
#include "RefsDialog.h"

using namespace std;

git_repository* OpenGitRepo(wstring dir) {
    string dirForGit = w2mb(dir);
    if (dirForGit.length() == 0) {
        *logFile << "Bad dir for Git: " << dir.c_str() << endl;
        return nullptr;
    }

    git_repository *repo;
    int error = git_repository_open_ext(&repo, dirForGit.c_str(), 0, nullptr);
    if (error < 0) {
        const git_error *e = giterr_last();
        *logFile << "libgit2 error " << error << "/" << e->klass << ": " << e->message << endl;
        return nullptr;
    }

    return repo;
}

static void FilterReferences(const Options &options, const char *ref, function<void (const char *)> filterOneRef) {
    const char *prefixes[] = { "refs/heads/", "refs/tags/" };
    for (int i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (StartsWith(ref, prefixes[i])) {
            filterOneRef(ref + strlen(prefixes[i]));
            return;
        }
    }
    const char *remotePrefix = "refs/remotes/";
    if (StartsWith(ref, remotePrefix)) {
        const char *remoteRef = ref + strlen(remotePrefix);
        filterOneRef(remoteRef);

        if (options.stripRemoteName) {
            const char *slashPtr = strchr(remoteRef, '/');
            assert(slashPtr != nullptr);
            filterOneRef(slashPtr + 1);
        }
        return;
    }
    // there are also "refs/stash", "refs/notes"
    *logFile << "Ignored ref = " << ref << endl;
}

static void ObtainSuitableRefsBy(const Options &options, git_repository *repo, vector<string> &suitableRefs, function<bool (const char *)> isSuitableRef) {
    git_reference_iterator *iter = NULL;
    int error = git_reference_iterator_new(&iter, repo);

    git_reference *ref;
    while (!(error = git_reference_next(&ref, iter))) {
        FilterReferences(options, git_reference_name(ref), [&suitableRefs, isSuitableRef](const char *refName) {
            if (isSuitableRef(refName)) {
                suitableRefs.push_back(string(refName));
            }
        });
    }
    assert(error == GIT_ITEROVER);
}

static void ObtainSuitableRefsByStrictPrefix(const Options &options, git_repository *repo, string currentPrefix, vector<string> &suitableRefs) {
    ObtainSuitableRefsBy(options, repo, suitableRefs, [&currentPrefix](const char *refName) -> bool {
        return StartsWith(refName, currentPrefix.c_str());
    });
}

/** Returns true for pairs like "cypok/arm/master" with prefix "cy/a/m". */
static bool RefMayBeEncodedByPartialPrefix(const char *ref, const char *prefix) {
    const char *p = prefix;
    const char *r = ref;
    for (;;) {
        if (*p == '\0') {
            return true;
        } else if (ispunct(*p) || isupper(*p)) {
            r = strchr(r, *p);
            if (r == nullptr) {
                return false;
            }
        } else {
            if (*p != *r) {
                return false;
            }
        }

        p++;
        r++;
    }
}

static void ObtainSuitableRefsByPartialPrefixes(const Options &options, git_repository *repo, string currentPrefix, vector<string> &suitableRefs) {
    ObtainSuitableRefsBy(options, repo, suitableRefs, [&currentPrefix](const char *refName) -> bool {
        return RefMayBeEncodedByPartialPrefix(refName, currentPrefix.c_str());
    });
}

static string FindCommonPrefix(vector<string> &suitableRefs) {
    Trie *trie = trie_create();
    for_each(suitableRefs.begin(), suitableRefs.end(), [trie](string s) {
        trie_add(trie, s);
    });
    string maxCommonPrefix = trie_get_common_prefix(trie);
    trie_free(trie);
    return maxCommonPrefix;
}

static string ObtainNextSuggestedSuffix(bool forwardSearch, string currentPrefix, string currentSuffix, vector<string> &suitableRefs) {
    size_t size = suitableRefs.size();
    size_t idx = distance(suitableRefs.begin(), find(suitableRefs.begin(), suitableRefs.end(), currentPrefix + currentSuffix));
    if (idx == size) {
        idx = forwardSearch ? 0 : (size - 1);
    } else {
        idx = (idx + (forwardSearch ? 1 : -1) + size) % size;
    }
    return DropPrefix(suitableRefs[idx], currentPrefix);
}

void TransformCmdLine(const Options &options, CmdLine &cmdLine, git_repository *repo) {
    string currentPrefix = w2mb(GetUserPrefix(cmdLine));
    *logFile << "User prefix = \"" << currentPrefix.c_str() << "\"" << endl;

    vector<string> suitableRefs;
    ObtainSuitableRefsByStrictPrefix(options, repo, currentPrefix, suitableRefs);

    if (suitableRefs.empty()) {
        ObtainSuitableRefsByPartialPrefixes(options, repo, currentPrefix, suitableRefs);
    }

    if (suitableRefs.empty()) {
        *logFile << "No suitable refs" << endl;
        return;
    }

    sort(suitableRefs.begin(), suitableRefs.end());
    suitableRefs.erase(unique(suitableRefs.begin(), suitableRefs.end()), suitableRefs.end());
    // TODO
    //if (!options.sortByName) {
    //    *logFile << "FIXME: sorting by time is not implemented yet" << endl;
    //}

    for_each(suitableRefs.begin(), suitableRefs.end(), [](string s) {
        *logFile << "Suitable ref: " << s.c_str() << endl;
    });

    string newPrefix = FindCommonPrefix(suitableRefs);
    *logFile << "Common prefix: " << newPrefix.c_str() << endl;

    if (newPrefix != currentPrefix) {
        ReplaceUserPrefix(cmdLine, mb2w(newPrefix));

    } else {
        string currentSuffix = w2mb(GetSuggestedSuffix(cmdLine));
        *logFile << "currentSuffix = \"" << currentSuffix.c_str() << "\"" << endl;

        if (options.showDialog) {
            // Yes, we show dialog even if there is only one suitable ref.
            *logFile << "Showing dialog..." << endl;
            string selectedRef = ShowRefsDialog(suitableRefs, currentPrefix + currentSuffix);
            *logFile << "Dialog closed, selectedRef = \"" << selectedRef.c_str() << "\"" << endl;
            if (!selectedRef.empty()) {
                // Use case: we iterate over branches with suggested suffixes
                // but then we understand that we do not remember branch name
                // and want to see them as dialog (via extra hotkey).
                // In this case we should drop last suggested suffix.
                ReplaceSuggestedSuffix(cmdLine, wstring(L""));

                ReplaceUserPrefix(cmdLine, mb2w(selectedRef));
            }

        } else {
            string newSuffix = ObtainNextSuggestedSuffix(options.suggestNextSuffix, currentPrefix, currentSuffix, suitableRefs);
            *logFile << "nextSuffx = \"" << newSuffix.c_str() << "\"" << endl;
            ReplaceSuggestedSuffix(cmdLine, mb2w(newSuffix));
        }
    }
}

#ifdef DEBUG
void LogicTest() {
    assert(RefMayBeEncodedByPartialPrefix("svn/trunk", "s/t"));
    assert(RefMayBeEncodedByPartialPrefix("foo/bar/qux", "f/b"));
    assert(RefMayBeEncodedByPartialPrefix("foo/bar/qux", "f/b/q"));
    assert(RefMayBeEncodedByPartialPrefix("foo/bar/qux", "f/ba/q"));
    assert(RefMayBeEncodedByPartialPrefix("foo/bar/qux", "f/bar/q"));
    assert(RefMayBeEncodedByPartialPrefix("foo/bar/qux", "foo/bar/qux"));
    assert(RefMayBeEncodedByPartialPrefix("foo/bar-qux", "f/b-q"));
    assert(RefMayBeEncodedByPartialPrefix("foo/barQux", "f/bQ"));

    assert(!RefMayBeEncodedByPartialPrefix("foo/bar/qux", "fo/baz/q"));
    assert(!RefMayBeEncodedByPartialPrefix("foo", "f/b"));
    assert(!RefMayBeEncodedByPartialPrefix("foo/", "f/b"));
    assert(!RefMayBeEncodedByPartialPrefix("foo/b", "f/bar"));
    assert(!RefMayBeEncodedByPartialPrefix("foo/bar/qux", "f/q"));
    assert(!RefMayBeEncodedByPartialPrefix("foo/bar-qux", "f/brq"));

    {
        vector<string> suitableRefs = { string("abcfoo"), string("abcxyz"), string("abcbar") };
        assert(string("bar") == ObtainNextSuggestedSuffix(true,  string("abc"), string("xyz"), suitableRefs));
        assert(string("foo") == ObtainNextSuggestedSuffix(true,  string("abc"), string("bar"), suitableRefs));
        assert(string("foo") == ObtainNextSuggestedSuffix(true,  string("abc"), string(""), suitableRefs));
        assert(string("foo") == ObtainNextSuggestedSuffix(false, string("abc"), string("xyz"), suitableRefs));
        assert(string("bar") == ObtainNextSuggestedSuffix(false, string("abc"), string("foo"), suitableRefs));
        assert(string("bar") == ObtainNextSuggestedSuffix(false,  string("abc"), string(""), suitableRefs));
    }
    {
        vector<string> suitableRefs = { string("abc"), string("abcxyz"), string("abcbar") };
        assert(string("")    == ObtainNextSuggestedSuffix(true,  string("abc"), string("bar"), suitableRefs));
        assert(string("xyz") == ObtainNextSuggestedSuffix(true,  string("abc"), string(""), suitableRefs));
        assert(string("bar") == ObtainNextSuggestedSuffix(false, string("abc"), string(""), suitableRefs));
        assert(string("")    == ObtainNextSuggestedSuffix(false, string("abc"), string("xyz"), suitableRefs));
    }
}
#endif
