/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

/* Patch app binary partial MAR file patch apply success test */

function run_test() {
  if (!setupTestCommon()) {
    return;
  }
  gTestFiles = gTestFilesPartialSuccess;
  gTestDirs = gTestDirsPartialSuccess;
  gCallbackBinFile = "exe0.exe";
  setupUpdaterTest(FILE_PARTIAL_MAR, false);
}

/**
 * Called after the call to setupUpdaterTest finishes.
 */
function setupUpdaterTestFinished() {
  runUpdate(STATE_SUCCEEDED, false, 0, true);
}

/**
 * Called after the call to runUpdate finishes.
 */
function runUpdateFinished() {
  checkPostUpdateAppLog();
}

/**
 * Called after the call to checkPostUpdateAppLog finishes.
 */
function checkPostUpdateAppLogFinished() {
  standardInit();
  checkPostUpdateRunningFile(true);
  checkFilesAfterUpdateSuccess(getApplyDirFile);
  checkUpdateLogContents(LOG_PARTIAL_SUCCESS);
  checkCallbackLog();
}
