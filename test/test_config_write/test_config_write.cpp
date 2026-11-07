// Host-native unit tests for config_write_policy.h (config file paths, landed writes).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "config_write_policy.h"
#include "path_safety_policy.h"
#include <map>
#include <string>

void setUp(void) {}
void tearDown(void) {}

void test_plain_config_path_is_live(void) {
  TEST_ASSERT_TRUE(configPathIsLive("/config.json"));
}

// pathSafe lets "." segments through, and littlefs resolves them to the file.
void test_dot_segment_spellings_are_live(void) {
  TEST_ASSERT_TRUE(pathSafePolicy("/./config.json"));
  TEST_ASSERT_TRUE(configPathIsLive("/./config.json"));
  TEST_ASSERT_TRUE(configPathIsLive("/././config.json"));
}

// littlefs 2.10+ answers NOTDIR for any path after a file, so these never
// reach /config.json and stay on the ordinary write path.
void test_path_after_the_file_is_not_live(void) {
  TEST_ASSERT_FALSE(configPathIsLive("/config.json/"));
  TEST_ASSERT_FALSE(configPathIsLive("/config.json/."));
  TEST_ASSERT_FALSE(configPathIsLive("/config.json/x"));
}

void test_other_files_are_not_live(void) {
  TEST_ASSERT_FALSE(configPathIsLive("/config.json.tmp"));
  TEST_ASSERT_FALSE(configPathIsLive("/config.jso"));
  TEST_ASSERT_FALSE(configPathIsLive("/Config.json"));
  TEST_ASSERT_FALSE(configPathIsLive("/.config.json"));
  TEST_ASSERT_FALSE(configPathIsLive("/./.config.json"));
  TEST_ASSERT_FALSE(configPathIsLive("/ca.crt"));
  TEST_ASSERT_FALSE(configPathIsLive("/scripts/config.json"));
  TEST_ASSERT_FALSE(configPathIsLive("/backup/config.json"));
}

void test_empty_root_and_null_are_not_live(void) {
  TEST_ASSERT_FALSE(configPathIsLive(nullptr));
  TEST_ASSERT_FALSE(configPathIsLive(""));
  TEST_ASSERT_FALSE(configPathIsLive("/"));
  TEST_ASSERT_FALSE(configPathIsLive("/."));
  TEST_ASSERT_FALSE(configPathIsLive("/./"));
}

// --- configWriteLanded -------------------------------------------------------

void test_write_that_landed_whole_is_ok(void) {
  TEST_ASSERT_TRUE(configWriteLanded(3000, 3000, 3000));
  TEST_ASSERT_TRUE(configWriteLanded(2, 2, 2));
}

// stdio took every byte but fclose hit NOSPC: the serializer count says done,
// the read-back says the tmp is empty or partial. Never renamed.
void test_buffered_count_without_the_bytes_on_disk_fails(void) {
  TEST_ASSERT_FALSE(configWriteLanded(3000, 0, 3000));
  TEST_ASSERT_FALSE(configWriteLanded(3000, 2048, 3000));
}

void test_short_serializer_count_fails(void) {
  TEST_ASSERT_FALSE(configWriteLanded(1024, 1024, 3000));
  TEST_ASSERT_FALSE(configWriteLanded(1024, 3000, 3000));
}

void test_disagreeing_or_empty_sizes_fail(void) {
  TEST_ASSERT_FALSE(configWriteLanded(3000, 3001, 3000));
  TEST_ASSERT_FALSE(configWriteLanded(3001, 3000, 3000));
  TEST_ASSERT_FALSE(configWriteLanded(0, 0, 0));
}

// --- fileWriteLanded / uploadTmpPath (#67) ------------------------------------

// An empty upload is a legitimate empty file, unlike an empty config.
void test_empty_file_lands_but_empty_config_does_not(void) {
  TEST_ASSERT_TRUE(fileWriteLanded(0, 0, 0));
  TEST_ASSERT_FALSE(configWriteLanded(0, 0, 0));
  TEST_ASSERT_TRUE(fileWriteLanded(40000, 40000, 40000));
  TEST_ASSERT_FALSE(fileWriteLanded(40000, 0, 40000));
  TEST_ASSERT_FALSE(fileWriteLanded(4096, 4096, 40000));
}

void test_upload_tmp_sits_beside_the_target(void) {
  char t[64];
  TEST_ASSERT_TRUE(uploadTmpPath(t, sizeof(t), "/rules.lua"));
  TEST_ASSERT_EQUAL_STRING("/.upload.tmp", t);
  TEST_ASSERT_TRUE(uploadTmpPath(t, sizeof(t), "/scripts/rules.lua"));
  TEST_ASSERT_EQUAL_STRING("/scripts/.upload.tmp", t);
}

void test_upload_tmp_refuses_bad_targets(void) {
  char t[64];
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), "/.upload.tmp"));
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), "/scripts/.upload.tmp"));
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), "/scripts/"));
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), "rules.lua"));
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), nullptr));
  TEST_ASSERT_FALSE(uploadTmpPath(nullptr, sizeof(t), "/a"));
  TEST_ASSERT_FALSE(uploadTmpPath(t, 0, "/a"));
}

// "/a/b/" plus ".upload.tmp" plus NUL is 17 bytes: 17 fits, 16 does not.
void test_upload_tmp_refuses_rather_than_clips(void) {
  char t[17];
  TEST_ASSERT_TRUE(uploadTmpPath(t, 17, "/a/b/file"));
  TEST_ASSERT_EQUAL_STRING("/a/b/.upload.tmp", t);
  TEST_ASSERT_FALSE(uploadTmpPath(t, 16, "/a/b/file"));
}

// --- FAT aliases of the staging names ------------------------------------------

// FAT compares case-blind and drops trailing dots and spaces, so each of these
// is the staging file itself. Staging over it would destroy the target.
void test_fat_spellings_of_the_staging_names_are_refused(void) {
  char t[64];
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), "/logs/.UPLOAD.TMP"));
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), "/logs/.upload.tmp."));
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), "/logs/.Upload.tmp "));
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), "/logs/.upload.tmp. . "));
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), "/logs/.upload.bak"));
  TEST_ASSERT_FALSE(uploadTmpPath(t, sizeof(t), "/logs/.UPLOAD.BAK."));
  TEST_ASSERT_FALSE(uploadStagePath(t, sizeof(t), "/logs/.upload.tmp", UPLOAD_BAK_NAME));
}

void test_near_miss_names_are_ordinary_targets(void) {
  char t[64];
  TEST_ASSERT_TRUE(uploadTmpPath(t, sizeof(t), "/logs/.upload.tmpx"));
  TEST_ASSERT_TRUE(uploadTmpPath(t, sizeof(t), "/logs/upload.tmp"));
  TEST_ASSERT_TRUE(uploadTmpPath(t, sizeof(t), "/logs/.upload.tm"));
  TEST_ASSERT_TRUE(uploadStagePath(t, sizeof(t), "/logs/a.csv", UPLOAD_BAK_NAME));
  TEST_ASSERT_EQUAL_STRING("/logs/.upload.bak", t);
  TEST_ASSERT_FALSE(uploadStagePath(t, sizeof(t), "/logs/a.csv", nullptr));
}

// --- fatCommit -----------------------------------------------------------------

static const char* T = "/d/.upload.tmp";
static const char* B = "/d/.upload.bak";
static const char* P = "/d/f.lua";

// In-memory FAT stand-in: rename refuses an existing target, up to two renames
// or one remove can fail, and a writer can recreate P once it is parked.
// One type only: gcc counts each fatCommit instance separately.
struct FakeFat {
  std::map<std::string, std::string> files;
  int renameFailAt = -1, renameFailAt2 = -1, removeFailAt = -1, renames = 0, removes = 0;
  bool recreateAfterPark = false;
  bool exists(const char* p) { return files.count(p) != 0; }
  bool remove(const char* p) {
    if (removes++ == removeFailAt) return false;
    return files.erase(p) != 0;
  }
  bool rename(const char* a, const char* b) {
    int n = renames++;
    if (n == renameFailAt || n == renameFailAt2) return false;
    if (!files.count(a) || files.count(b)) return false;
    files[b] = files[a];
    files.erase(a);
    if (recreateAfterPark && std::string(b) == B) files[P] = "row";
    return true;
  }
  bool holds(const std::string& content) {
    for (auto& kv : files) if (kv.second == content) return true;
    return false;
  }
};

void test_fat_commit_replaces_and_cleans_up(void) {
  FakeFat fs;
  fs.files[P] = "old";
  fs.files[T] = "new";
  TEST_ASSERT_EQUAL(FAT_COMMIT_OK, fatCommit(fs, T, B, P));
  TEST_ASSERT_EQUAL_STRING("new", fs.files[P].c_str());
  TEST_ASSERT_EQUAL(1, (int)fs.files.size());
}

void test_fat_commit_to_a_new_name(void) {
  FakeFat fs;
  fs.files[T] = "new";
  TEST_ASSERT_EQUAL(FAT_COMMIT_OK, fatCommit(fs, T, B, P));
  TEST_ASSERT_EQUAL_STRING("new", fs.files[P].c_str());
}

// Every rename failure: the old content is never the one that is lost.
void test_fat_commit_never_loses_the_old_file(void) {
  for (int failAt = 0; failAt < 3; failAt++) {
    FakeFat fs;
    fs.files[P] = "old";
    fs.files[T] = "new";
    fs.renameFailAt = failAt;
    FatCommit r = fatCommit(fs, T, B, P);
    TEST_ASSERT_TRUE(fs.holds("old") || r == FAT_COMMIT_OK);
    if (r == FAT_COMMIT_KEPT_OLD) TEST_ASSERT_EQUAL_STRING("old", fs.files[P].c_str());
  }
}

// The second rename fails and so does the restore: both copies stay.
void test_fat_commit_strands_both_copies_rather_than_delete_one(void) {
  FakeFat fs;
  fs.files[P] = "old";
  fs.files[T] = "new";
  fs.renameFailAt = 1;   // tmp to path
  fs.renameFailAt2 = 2;  // bak back to path
  TEST_ASSERT_EQUAL(FAT_COMMIT_STRANDED, fatCommit(fs, T, B, P));
  TEST_ASSERT_EQUAL_STRING("old", fs.files[B].c_str());
  TEST_ASSERT_EQUAL_STRING("new", fs.files[T].c_str());
}

// Something recreates the target between park and rename (an SD log append):
// the rename then fails on FR_EXIST and nothing is deleted.
void test_fat_commit_survives_a_writer_recreating_the_target(void) {
  FakeFat fs;
  fs.files[P] = "old";
  fs.files[T] = "new";
  fs.recreateAfterPark = true;
  TEST_ASSERT_EQUAL(FAT_COMMIT_STRANDED, fatCommit(fs, T, B, P));
  TEST_ASSERT_TRUE(fs.holds("old"));
  TEST_ASSERT_TRUE(fs.holds("new"));
}

void test_fat_commit_reports_a_bak_it_could_not_remove(void) {
  FakeFat fs;
  fs.files[P] = "old";
  fs.files[T] = "new";
  fs.removeFailAt = 1;  // remove #0 is the stale-bak sweep, #1 the cleanup
  TEST_ASSERT_EQUAL(FAT_COMMIT_OK_BAK_LEFT, fatCommit(fs, T, B, P));
  TEST_ASSERT_EQUAL_STRING("new", fs.files[P].c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_plain_config_path_is_live);
  RUN_TEST(test_dot_segment_spellings_are_live);
  RUN_TEST(test_path_after_the_file_is_not_live);
  RUN_TEST(test_other_files_are_not_live);
  RUN_TEST(test_empty_root_and_null_are_not_live);
  RUN_TEST(test_write_that_landed_whole_is_ok);
  RUN_TEST(test_buffered_count_without_the_bytes_on_disk_fails);
  RUN_TEST(test_short_serializer_count_fails);
  RUN_TEST(test_disagreeing_or_empty_sizes_fail);
  RUN_TEST(test_empty_file_lands_but_empty_config_does_not);
  RUN_TEST(test_upload_tmp_sits_beside_the_target);
  RUN_TEST(test_upload_tmp_refuses_bad_targets);
  RUN_TEST(test_upload_tmp_refuses_rather_than_clips);
  RUN_TEST(test_fat_spellings_of_the_staging_names_are_refused);
  RUN_TEST(test_near_miss_names_are_ordinary_targets);
  RUN_TEST(test_fat_commit_replaces_and_cleans_up);
  RUN_TEST(test_fat_commit_to_a_new_name);
  RUN_TEST(test_fat_commit_never_loses_the_old_file);
  RUN_TEST(test_fat_commit_strands_both_copies_rather_than_delete_one);
  RUN_TEST(test_fat_commit_survives_a_writer_recreating_the_target);
  RUN_TEST(test_fat_commit_reports_a_bak_it_could_not_remove);
  return UNITY_END();
}
