# RUN: %{lit} %{inputs}/discovery | FileCheck --check-prefix=CHECK-BASIC %s
# CHECK-BASIC: Testing: 5 tests

# Check that regex-filtering works
#
# RUN: %{lit} --filter 'o[a-z]e' %{inputs}/discovery | FileCheck --check-prefix=CHECK-FILTER %s
# CHECK-FILTER: Testing: 2 of 5 tests

# Check that regex-filtering based on environment variables work.
#
# RUN: env LIT_FILTER='o[a-z]e' %{lit} %{inputs}/discovery | FileCheck --check-prefix=CHECK-FILTER-ENV %s
# CHECK-FILTER-ENV: Testing: 2 of 5 tests


# Check that maximum counts work
#
# RUN: %{lit} --max-tests 3 %{inputs}/discovery | FileCheck --check-prefix=CHECK-MAX %s
# CHECK-MAX: Testing: 3 of 5 tests


# Check that sharding partitions the testsuite in a way that distributes the
# rounding error nicely (i.e. 5/3 => 2 2 1, not 1 1 3 or whatever)
#
# RUN: %{lit} --num-shards 3 --run-shard 1 %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD0-ERR < %t.err %s
# RUN: FileCheck --check-prefix=CHECK-SHARD0-OUT < %t.out %s
# CHECK-SHARD0-ERR: note: Selecting shard 1/3 = size 2/5 = tests #(3*k)+1 = [1, 4]
# CHECK-SHARD0-OUT: Testing: 2 of 5 tests
#
# RUN: %{lit} --num-shards 3 --run-shard 2 %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD1-ERR < %t.err %s
# RUN: FileCheck --check-prefix=CHECK-SHARD1-OUT < %t.out %s
# CHECK-SHARD1-ERR: note: Selecting shard 2/3 = size 2/5 = tests #(3*k)+2 = [2, 5]
# CHECK-SHARD1-OUT: Testing: 2 of 5 tests
#
# RUN: %{lit} --num-shards 3 --run-shard 3 %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD2-ERR < %t.err %s
# RUN: FileCheck --check-prefix=CHECK-SHARD2-OUT < %t.out %s
# CHECK-SHARD2-ERR: note: Selecting shard 3/3 = size 1/5 = tests #(3*k)+3 = [3]
# CHECK-SHARD2-OUT: Testing: 1 of 5 tests


# Check that sharding via env vars works.
#
# RUN: env LIT_NUM_SHARDS=3 LIT_RUN_SHARD=1 %{lit} %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD0-ENV-ERR < %t.err %s
# RUN: FileCheck --check-prefix=CHECK-SHARD0-ENV-OUT < %t.out %s
# CHECK-SHARD0-ENV-ERR: note: Selecting shard 1/3 = size 2/5 = tests #(3*k)+1 = [1, 4]
# CHECK-SHARD0-ENV-OUT: Testing: 2 of 5 tests
#
# RUN: env LIT_NUM_SHARDS=3 LIT_RUN_SHARD=2 %{lit} %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD1-ENV-ERR < %t.err %s
# RUN: FileCheck --check-prefix=CHECK-SHARD1-ENV-OUT < %t.out %s
# CHECK-SHARD1-ENV-ERR: note: Selecting shard 2/3 = size 2/5 = tests #(3*k)+2 = [2, 5]
# CHECK-SHARD1-ENV-OUT: Testing: 2 of 5 tests
#
# RUN: env LIT_NUM_SHARDS=3 LIT_RUN_SHARD=3 %{lit} %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD2-ENV-ERR < %t.err %s
# RUN: FileCheck --check-prefix=CHECK-SHARD2-ENV-OUT < %t.out %s
# CHECK-SHARD2-ENV-ERR: note: Selecting shard 3/3 = size 1/5 = tests #(3*k)+3 = [3]
# CHECK-SHARD2-ENV-OUT: Testing: 1 of 5 tests


# Check that providing more shards than tests results in 1 test per shard
# until we run out, then 0.
#
# RUN: %{lit} --num-shards 100 --run-shard 2 %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD-BIG-ERR1 < %t.err %s
# RUN: FileCheck --check-prefix=CHECK-SHARD-BIG-OUT1 < %t.out %s
# CHECK-SHARD-BIG-ERR1: note: Selecting shard 2/100 = size 1/5 = tests #(100*k)+2 = [2]
# CHECK-SHARD-BIG-OUT1: Testing: 1 of 5 tests
#
# RUN: %{lit} --num-shards 100 --run-shard 6 %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD-BIG-ERR2 < %t.err %s
# RUN: FileCheck --check-prefix=CHECK-SHARD-BIG-OUT2 < %t.out %s
# CHECK-SHARD-BIG-ERR2: note: Selecting shard 6/100 = size 0/5 = tests #(100*k)+6 = []
# CHECK-SHARD-BIG-OUT2: Testing: 0 of 5 tests
#
# RUN: %{lit} --num-shards 100 --run-shard 50 %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD-BIG-ERR3 < %t.err %s
# RUN: FileCheck --check-prefix=CHECK-SHARD-BIG-OUT3 < %t.out %s
# CHECK-SHARD-BIG-ERR3: note: Selecting shard 50/100 = size 0/5 = tests #(100*k)+50 = []
# CHECK-SHARD-BIG-OUT3: Testing: 0 of 5 tests


# Check that range constraints are enforced
#
# RUN: not %{lit} --num-shards 0 --run-shard 2 %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD-ERR < %t.err %s
# CHECK-SHARD-ERR: error: argument --num-shards: requires positive integer, but found '0'
#
# RUN: not %{lit} --num-shards 3 --run-shard 4 %{inputs}/discovery >%t.out 2>%t.err
# RUN: FileCheck --check-prefix=CHECK-SHARD-ERR2 < %t.err %s
# CHECK-SHARD-ERR2: error: --run-shard must be between 1 and --num-shards (inclusive)
