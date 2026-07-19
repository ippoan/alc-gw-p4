// linux target で動く unity ランナー。CI (ci.yml の host_test_rtsp ジョブ) は
// この実行ファイルの終了コードでテストの成否を判定するため、失敗数を
// そのまま exit code に反映する。
#include <stdlib.h>

#include "unity.h"

void app_main(void) {
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
