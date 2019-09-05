#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(73)

--!./tcltestrunner.lua
-- 2010 August 27
--
-- The author disclaims copyright to this source code.  In place of
-- a legal notice, here is a blessing:
--
--    May you do good and not evil.
--    May you find forgiveness for yourself and forgive others.
--    May you share freely, never taking more than you give.
--
-------------------------------------------------------------------------
--
-- Testing of function factoring and the sql_DETERMINISTIC flag.
--

-- Verify that constant string expressions that get factored into initializing
-- code are not reused between function parameters and other values in the
-- VDBE program, as the function might have changed the encoding.
--
test:do_execsql_test(
    "func5-1.1",
    [[
        CREATE TABLE t1(x INT PRIMARY KEY,a TEXT,b TEXT,c INT );
        INSERT INTO t1 VALUES(1,'ab','cd',1);
        INSERT INTO t1 VALUES(2,'gh','ef',5);
        INSERT INTO t1 VALUES(3,'pqr','fuzzy',99);
        INSERT INTO t1 VALUES(4,'abcdefg','xy',22);
        INSERT INTO t1 VALUES(5,'shoe','mayer',2953);
        SELECT x FROM t1 WHERE c=position(b, 'abcdefg') OR a='abcdefg' ORDER BY +x;
    ]], {
        -- <func5-1.1>
        2, 4
        -- </func5-1.1>
    })

test:do_execsql_test(
    "func5-1.2",
    [[
        SELECT x FROM t1 WHERE a='abcdefg' OR c=position(b, 'abcdefg') ORDER BY +x;
    ]], {
        -- <func5-1.1>
        2, 4
        -- </func5-1.1>
    })

-- Verify that sql_DETERMINISTIC functions get factored out of the
-- evaluation loop whereas non-deterministic functions do not.  counter1()
-- is marked as non-deterministic and so is not factored out of the loop,
-- and it really is non-deterministic, returning a different result each
-- time.  But counter2() is marked as deterministic, so it does get factored
-- out of the loop.  counter2() has the same implementation as counter1(),
-- returning a different result on each invocation, but because it is 
-- only invoked once outside of the loop, it appears to return the same
-- result multiple times.
--
test:do_execsql_test(
    "func5-2.1",
    [[
        CREATE TABLE t2(x  INT PRIMARY KEY,y INT );
        INSERT INTO t2 VALUES(1,2),(3,4),(5,6),(7,8);
        SELECT x, y FROM t2 WHERE x+5=5+x ORDER BY +x;
    ]], {
        -- <func5-2.1>
        1, 2, 3, 4, 5, 6, 7, 8
        -- </func5-2.1>
    })

global_counter = 0

box.schema.func.create('COUNTER1', {language = 'Lua', is_deterministic = false,
                       param_list = {'any'}, returns = 'integer',
                       exports = {'SQL', 'LUA'},
                       body = [[
                           function(str)
                               global_counter = global_counter + 1
                               return global_counter
                           end
                       ]]})

box.schema.func.create('COUNTER2', {language = 'Lua', is_deterministic = true,
                       param_list = {'any'}, returns = 'integer',
                       exports = {'SQL', 'LUA'},
                       body = [[
                           function(str)
                                   global_counter = global_counter + 1
                                   return global_counter
                               end
                       ]]})

test:do_execsql_test(
    "func5-2.2",
    [[
        SELECT x, y FROM t2 WHERE x+counter1('hello')=counter1('hello')+x ORDER BY +x;
    ]], {
        -- <func5-2.2>
        -- </func5-2.2>
    })

test:do_execsql_test(
    "func5-2.3",
    [[
        SELECT x, y FROM t2 WHERE x+counter2('hello')=counter2('hello')+x ORDER BY +x;
    ]], {
        -- <func5-2.2>
        1, 2, 3, 4, 5, 6, 7, 8
        -- </func5-2.2>
    })

-- The following tests ensures that GREATEST() and LEAST()
-- functions raise error if argument's collations are incompatible.

test:do_catchsql_test(
    "func-5-3.1",
    [[
        SELECT GREATEST('a' COLLATE "unicode", 'A' COLLATE "unicode_ci");
    ]],
    {
        -- <func5-3.1>
        1, "Illegal mix of collations"
        -- </func5-3.1>
    }
)

test:do_catchsql_test(
    "func-5-3.2",
    [[
        CREATE TABLE test1 (s1 VARCHAR(5) PRIMARY KEY COLLATE "unicode");
        CREATE TABLE test2 (s2 VARCHAR(5) PRIMARY KEY COLLATE "unicode_ci");
        INSERT INTO test1 VALUES ('a');
        INSERT INTO test2 VALUES ('a');
        SELECT GREATEST(s1, s2) FROM test1 JOIN test2;
    ]],
    {
        -- <func5-3.2>
        1, "Illegal mix of collations"
        -- </func5-3.2>
    }
)

test:do_catchsql_test(
    "func-5-3.3",
    [[
        SELECT GREATEST ('abc', 'asd' COLLATE "binary", 'abc' COLLATE "unicode")
    ]],
    {
        -- <func5-3.3>
        1, "Illegal mix of collations"
        -- </func5-3.3>
    }
)

test:do_execsql_test(
    "func-5-3.4",
    [[
        SELECT GREATEST (s1, 'asd' COLLATE "binary", s2) FROM test1 JOIN test2;
    ]], {
        -- <func5-3.4>
        "asd"
        -- </func5-3.4>
    }
)

test:do_catchsql_test(
    "func-5.3.5",
    [[
        CREATE TABLE test3 (s3 VARCHAR(5) PRIMARY KEY COLLATE "unicode");
        CREATE TABLE test4 (s4 VARCHAR(5) PRIMARY KEY COLLATE "unicode");
        CREATE TABLE test5 (s5 VARCHAR(5) PRIMARY KEY COLLATE "binary");
        INSERT INTO test3 VALUES ('a');
        INSERT INTO test4 VALUES ('a');
        INSERT INTO test5 VALUES ('a');
        SELECT GREATEST(s3, s4, s5) FROM test3 JOIN test4 JOIN test5;
    ]],
    {
        -- <func5-3.5>
        1, "Illegal mix of collations"
        -- </func5-3.5>
    }
)

test:do_catchsql_test(
    "func-5-3.6",
    [[
        SELECT LEAST('a' COLLATE "unicode", 'A' COLLATE "unicode_ci");
    ]],
    {
        -- <func5-3.6>
        1, "Illegal mix of collations"
        -- </func5-3.6>
    }
)

test:do_catchsql_test(
    "func-5-3.7",
    [[
        SELECT LEAST(s1, s2) FROM test1 JOIN test2;
    ]],
    {
        -- <func5-3.7>
        1, "Illegal mix of collations"
        -- </func5-3.7>
    }
)

test:do_catchsql_test(
    "func-5-3.8",
    [[
        SELECT LEAST('abc', 'asd' COLLATE "binary", 'abc' COLLATE "unicode")
    ]],
    {
        -- <func5-3.8>
        1, "Illegal mix of collations"
        -- </func5-3.8>
    }
)

test:do_execsql_test(
    "func-5-3.9",
    [[
        SELECT LEAST(s1, 'asd' COLLATE "binary", s2) FROM test1 JOIN test2;
    ]], {
        -- <func5-3.9>
        "a"
        -- </func5-3.9>
    }
)

test:do_catchsql_test(
    "func-5.3.10",
    [[
        SELECT LEAST(s3, s4, s5) FROM test3 JOIN test4 JOIN test5;
    ]],
    {
        -- <func5-3.10>
        1, "Illegal mix of collations"
        -- <func5-3.10>
    }
)

-- Order of arguments of LEAST/GREATEST functions doesn't affect
-- the result: boolean is always less than numbers, which
-- are less than strings.
--
test:do_execsql_test(
    "func-5-4.1",
    [[
        SELECT GREATEST (false, 'STR', 1, 0.5);
    ]], { "STR" } )

test:do_execsql_test(
    "func-5-4.2",
    [[
        SELECT GREATEST ('STR', 1, 0.5, false);
    ]], { "STR" } )

test:do_execsql_test(
    "func-5-4.3",
    [[
        SELECT LEAST('STR', 1, 0.5, false);
    ]], { false } )

test:do_execsql_test(
    "func-5-4.4",
    [[
        SELECT LEAST(false, 'STR', 1, 0.5);
    ]], { false } )

-- gh-4453: GREATEST()/LEAST() require at least two arguments
-- be passed to these functions.
--
test:do_catchsql_test(
    "func-5-5.1",
    [[
        SELECT LEAST(false);
    ]], { 1, "Wrong number of arguments is passed to LEAST(): expected at least two, got 1" } )

test:do_catchsql_test(
    "func-5-5.2",
    [[
        SELECT GREATEST('abc');
    ]], { 1, "Wrong number of arguments is passed to GREATEST(): expected at least two, got 1" } )

test:do_catchsql_test(
    "func-5-5.3",
    [[
        SELECT LEAST();
    ]], { 1, "Wrong number of arguments is passed to LEAST(): expected at least two, got 0" } )

box.func.COUNTER1:drop()
box.func.COUNTER2:drop()

--
-- gh-4159: Make sure that functions accept arguments with right
-- types.
--
test:do_execsql_test(
    "func-5-6.1",
    [[
        SELECT LENGTH('some text'), LENGTH(X'1020304050'), LENGTH(NULL);
    ]], {
        -- <func5-6.1>
        9, 5, ""
        -- </func5-6.1>
    }
)

test:do_catchsql_test(
    "func-5-6.2",
    [[
        SELECT LENGTH(true);
    ]],
    {
        -- <func5-6.2>
        1, "Inconsistent types: expected TEXT or VARBINARY got BOOLEAN"
        -- </func5-6.2>
    }
)

test:do_catchsql_test(
    "func-5-6.3",
    [[
        SELECT LENGTH(false);
    ]],
    {
        -- <func5-6.3>
        1, "Inconsistent types: expected TEXT or VARBINARY got BOOLEAN"
        -- </func5-6.3>
    }
)

test:do_catchsql_test(
    "func-5-6.4",
    [[
        SELECT LENGTH(12345);
    ]],
    {
        -- <func5-6.4>
        1, "Inconsistent types: expected TEXT or VARBINARY got UNSIGNED"
        -- </func5-6.4>
    }
)

test:do_catchsql_test(
    "func-5-6.5",
    [[
        SELECT LENGTH(-12345);
    ]],
    {
        -- <func5-6.5>
        1, "Inconsistent types: expected TEXT or VARBINARY got INTEGER"
        -- </func5-6.5>
    }
)

test:do_catchsql_test(
    "func-5-6.6",
    [[
        SELECT LENGTH(123.45);
    ]],
    {
        -- <func5-6.6>
        1, "Inconsistent types: expected TEXT or VARBINARY got REAL"
        -- </func5-6.6>
    }
)

test:do_execsql_test(
    "func-5-6.7",
    [[
        SELECT ABS(12345), ABS(-12345), ABS(123.45), ABS(-123.45);
    ]], {
        -- <func5-6.9>
        12345, 12345, 123.45, 123.45
        -- </func5-6.9>
    }
)

test:do_catchsql_test(
    "func-5-6.8",
    [[
        SELECT ABS('12345');
    ]],
    {
        -- <func5-6.8>
        1, "Inconsistent types: expected NUMBER got TEXT"
        -- </func5-6.8>
    }
)

test:do_catchsql_test(
    "func-5-6.9",
    [[
        SELECT ABS(X'102030');
    ]],
    {
        -- <func5-6.9>
        1, "Inconsistent types: expected NUMBER got VARBINARY"
        -- </func5-6.9>
    }
)

test:do_catchsql_test(
    "func-5-6.10",
    [[
        SELECT ABS(false);
    ]],
    {
        -- <func5-6.10>
        1, "Inconsistent types: expected NUMBER got BOOLEAN"
        -- </func5-6.10>
    }
)

test:do_catchsql_test(
    "func-5-6.11",
    [[
        SELECT ABS(true);
    ]],
    {
        -- <func5-6.11>
        1, "Inconsistent types: expected NUMBER got BOOLEAN"
        -- </func5-6.11>
    }
)

test:do_execsql_test(
    "func-5-6.12",
    [[
        SELECT UPPER('sOmE tExt'), LOWER('sOmE tExt'), UPPER(NULL), LOWER(NULL);
    ]], {
        -- <func5-6.12>
        "SOME TEXT", "some text", "", ""
        -- </func5-6.12>
    }
)

test:do_catchsql_test(
    "func-5-6.13",
    [[
        SELECT UPPER(true);
    ]],
    {
        -- <func5-6.13>
        1, "Inconsistent types: expected TEXT got BOOLEAN"
        -- </func5-6.13>
    }
)

test:do_catchsql_test(
    "func-5-6.14",
    [[
        SELECT UPPER(false);
    ]],
    {
        -- <func5-6.14>
        1, "Inconsistent types: expected TEXT got BOOLEAN"
        -- </func5-6.14>
    }
)

test:do_catchsql_test(
    "func-5-6.15",
    [[
        SELECT UPPER(12345);
    ]],
    {
        -- <func5-6.15>
        1, "Inconsistent types: expected TEXT got UNSIGNED"
        -- </func5-6.15>
    }
)

test:do_catchsql_test(
    "func-5-6.16",
    [[
        SELECT UPPER(-12345);
    ]],
    {
        -- <func5-6.16>
        1, "Inconsistent types: expected TEXT got INTEGER"
        -- </func5-6.16>
    }
)

test:do_catchsql_test(
    "func-5-6.17",
    [[
        SELECT UPPER(123.45);
    ]],
    {
        -- <func5-6.17>
        1, "Inconsistent types: expected TEXT got REAL"
        -- </func5-6.17>
    }
)

test:do_catchsql_test(
    "func-5-6.18",
    [[
        SELECT UPPER(X'102030');
    ]],
    {
        -- <func5-6.18>
        1, "Inconsistent types: expected TEXT got VARBINARY"
        -- </func5-6.18>
    }
)

test:do_catchsql_test(
    "func-5-6.19",
    [[
        SELECT LOWER(true);
    ]],
    {
        -- <func5-6.19>
        1, "Inconsistent types: expected TEXT got BOOLEAN"
        -- </func5-6.19>
    }
)

test:do_catchsql_test(
    "func-5-6.20",
    [[
        SELECT LOWER(false);
    ]],
    {
        -- <func5-6.20>
        1, "Inconsistent types: expected TEXT got BOOLEAN"
        -- </func5-6.20>
    }
)

test:do_catchsql_test(
    "func-5-6.21",
    [[
        SELECT LOWER(12345);
    ]],
    {
        -- <func5-6.21>
        1, "Inconsistent types: expected TEXT got UNSIGNED"
        -- </func5-6.21>
    }
)

test:do_catchsql_test(
    "func-5-6.22",
    [[
        SELECT LOWER(-12345);
    ]],
    {
        -- <func5-6.22>
        1, "Inconsistent types: expected TEXT got INTEGER"
        -- </func5-6.22>
    }
)

test:do_catchsql_test(
    "func-5-6.23",
    [[
        SELECT LOWER(123.45);
    ]],
    {
        -- <func5-6.23>
        1, "Inconsistent types: expected TEXT got REAL"
        -- </func5-6.23>
    }
)

test:do_catchsql_test(
    "func-5-6.24",
    [[
        SELECT LOWER(X'102030');
    ]],
    {
        -- <func5-6.24>
        1, "Inconsistent types: expected TEXT got VARBINARY"
        -- </func5-6.24>
    }
)

test:do_execsql_test(
    "func-5-6.25",
    [[
        SELECT LENGTH(a), TYPEOF(a) from (SELECT RANDOMBLOB(12) AS a);
    ]], {
        -- <func5-6.25>
        12, "varbinary"
        -- </func5-6.25>
    }
)

test:do_catchsql_test(
    "func-5-6.26",
    [[
        SELECT RANDOMBLOB(true);
    ]],
    {
        -- <func5-6.26>
        1, "Inconsistent types: expected UNSIGNED got BOOLEAN"
        -- </func5-6.26>
    }
)

test:do_catchsql_test(
    "func-5-6.27",
    [[
        SELECT RANDOMBLOB(false);
    ]],
    {
        -- <func5-6.27>
        1, "Inconsistent types: expected UNSIGNED got BOOLEAN"
        -- </func5-6.27>
    }
)

test:do_catchsql_test(
    "func-5-6.28",
    [[
        SELECT RANDOMBLOB(-12345);
    ]],
    {
        -- <func5-6.28>
        1, "Inconsistent types: expected UNSIGNED got INTEGER"
        -- </func5-6.28>
    }
)

test:do_catchsql_test(
    "func-5-6.29",
    [[
        SELECT RANDOMBLOB(123.45);
    ]],
    {
        -- <func5-6.29>
        1, "Inconsistent types: expected UNSIGNED got REAL"
        -- </func5-6.29>
    }
)

test:do_catchsql_test(
    "func-5-6.30",
    [[
        SELECT RANDOMBLOB('102030');
    ]],
    {
        -- <func5-6.30>
        1, "Inconsistent types: expected UNSIGNED got TEXT"
        -- </func5-6.30>
    }
)

test:do_catchsql_test(
    "func-5-6.31",
    [[
        SELECT RANDOMBLOB(X'102030');
    ]],
    {
        -- <func5-6.31>
        1, "Inconsistent types: expected UNSIGNED got VARBINARY"
        -- </func5-6.31>
    }
)

test:do_execsql_test(
    "func-5-6.32",
    [[
        SELECT CHAR(70, NULL, 80, NULL, 90);
    ]], {
        -- <func5-6.32>
        "F\0P\0Z"
        -- </func5-6.32>
    }
)

test:do_catchsql_test(
    "func-5-6.33",
    [[
        SELECT CHAR(true);
    ]],
    {
        -- <func5-6.33>
        1, "Inconsistent types: expected UNSIGNED got BOOLEAN"
        -- </func5-6.33>
    }
)

test:do_catchsql_test(
    "func-5-6.34",
    [[
        SELECT CHAR(false);
    ]],
    {
        -- <func5-6.34>
        1, "Inconsistent types: expected UNSIGNED got BOOLEAN"
        -- </func5-6.34>
    }
)

test:do_catchsql_test(
    "func-5-6.35",
    [[
        SELECT CHAR(-12345);
    ]],
    {
        -- <func5-6.35>
        1, "Inconsistent types: expected UNSIGNED got INTEGER"
        -- </func5-6.35>
    }
)

test:do_catchsql_test(
    "func-5-6.36",
    [[
        SELECT CHAR(123.45);
    ]],
    {
        -- <func5-6.36>
        1, "Inconsistent types: expected UNSIGNED got REAL"
        -- </func5-6.36>
    }
)

test:do_catchsql_test(
    "func-5-6.37",
    [[
        SELECT CHAR('102030');
    ]],
    {
        -- <func5-6.37>
        1, "Inconsistent types: expected UNSIGNED got TEXT"
        -- </func5-6.37>
    }
)

test:do_catchsql_test(
    "func-5-6.38",
    [[
        SELECT CHAR(X'102030');
    ]],
    {
        -- <func5-6.38>
        1, "Inconsistent types: expected UNSIGNED got VARBINARY"
        -- </func5-6.38>
    }
)

test:do_execsql_test(
    "func-5-6.39",
    [[
        SELECT HEX('some text'), HEX(X'1020304050'), HEX(NULL);
    ]], {
        -- <func5-6.39>
        "736F6D652074657874", "1020304050", ""
        -- </func5-6.39>
    }
)

test:do_catchsql_test(
    "func-5-6.40",
    [[
        SELECT HEX(true);
    ]],
    {
        -- <func5-6.40>
        1, "Inconsistent types: expected TEXT or VARBINARY got BOOLEAN"
        -- </func5-6.40>
    }
)

test:do_catchsql_test(
    "func-5-6.41",
    [[
        SELECT HEX(false);
    ]],
    {
        -- <func5-6.41>
        1, "Inconsistent types: expected TEXT or VARBINARY got BOOLEAN"
        -- </func5-6.41>
    }
)

test:do_catchsql_test(
    "func-5-6.42",
    [[
        SELECT HEX(12345);
    ]],
    {
        -- <func5-6.42>
        1, "Inconsistent types: expected TEXT or VARBINARY got UNSIGNED"
        -- </func5-6.42>
    }
)

test:do_catchsql_test(
    "func-5-6.43",
    [[
        SELECT HEX(-12345);
    ]],
    {
        -- <func5-6.43>
        1, "Inconsistent types: expected TEXT or VARBINARY got INTEGER"
        -- </func5-6.43>
    }
)

test:do_catchsql_test(
    "func-5-6.44",
    [[
        SELECT HEX(123.45);
    ]],
    {
        -- <func5-6.44>
        1, "Inconsistent types: expected TEXT or VARBINARY got REAL"
        -- </func5-6.44>
    }
)

test:do_execsql_test(
    "func-5-6.45",
    [[
        SELECT SOUNDEX('some text'), SOUNDEX(NULL);
    ]], {
        -- <func5-6.45>
        "S532","?000"
        -- </func5-6.45>
    }
)

test:do_catchsql_test(
    "func-5-6.46",
    [[
        SELECT SOUNDEX(true);
    ]],
    {
        -- <func5-6.46>
        1, "Inconsistent types: expected TEXT got BOOLEAN"
        -- </func5-6.46>
    }
)

test:do_catchsql_test(
    "func-5-6.47",
    [[
        SELECT SOUNDEX(false);
    ]],
    {
        -- <func5-6.47>
        1, "Inconsistent types: expected TEXT got BOOLEAN"
        -- </func5-6.47>
    }
)

test:do_catchsql_test(
    "func-5-6.48",
    [[
        SELECT SOUNDEX(12345);
    ]],
    {
        -- <func5-6.48>
        1, "Inconsistent types: expected TEXT got UNSIGNED"
        -- </func5-6.48>
    }
)

test:do_catchsql_test(
    "func-5-6.49",
    [[
        SELECT SOUNDEX(-12345);
    ]],
    {
        -- <func5-6.49>
        1, "Inconsistent types: expected TEXT got INTEGER"
        -- </func5-6.49>
    }
)

test:do_catchsql_test(
    "func-5-6.50",
    [[
        SELECT SOUNDEX(123.45);
    ]],
    {
        -- <func5-6.50>
        1, "Inconsistent types: expected TEXT got REAL"
        -- </func5-6.50>
    }
)

test:do_catchsql_test(
    "func-5-6.51",
    [[
        SELECT SOUNDEX(X'102030');
    ]],
    {
        -- <func5-6.51>
        1, "Inconsistent types: expected TEXT got VARBINARY"
        -- </func5-6.51>
    }
)

test:finish_test()
