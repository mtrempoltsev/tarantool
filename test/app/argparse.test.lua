-- internal argparse test
test_run = require('test_run').new()
test_run:cmd("push filter ".."'\\.lua.*:[0-9]+: ' to '.lua:<line>\"]: '")

argparse = require('internal.argparse').parse

-- test with empty arguments and options
argparse()
-- test with command name (should be excluded)
argparse({[0] = 'tarantoolctl', 'start', 'instance'})
-- test long option
argparse({'tarantoolctl', 'start', 'instance', '--start'})
argparse({'tarantoolctl', 'start', 'instance', '--start', '--stop'})
argparse({'tarantoolctl', 'start', 'instance', '--start', '--stop', '--stop'})
argparse({'tarantoolctl', 'start', 'instance', '--start', '--stop', '--stop'})
argparse({'tarantoolctl', 'start', 'instance', '-baobab'})
argparse({'tarantoolctl', 'start', 'instance', '-vovov'})
argparse({'tarantoolctl', 'start', 'instance', '--start=lalochka'})
argparse({'tarantoolctl', 'start', 'instance', '--start', 'lalochka'})
argparse({'tarantoolctl', 'start', 'instance', '--start', '--', 'lalochka'})
argparse({'tarantoolctl', 'start', 'instance', '--start', '-', 'lalochka'})
argparse({'--verh=42'}, {{'verh', 'number'}})
argparse({'--verh=42'}, {{'verh', 'number+'}})
argparse({'--verh=42'}, {{'verh', 'string'}})
argparse({'--verh=42'}, {{'verh', 'string+'}})
argparse({'--verh=42'}, {{'verh'}})
argparse({'--verh=42'}, {'verh'})
argparse({'--verh=42'}, {{'verh', 'boolean'}})
argparse({'--verh=42'}, {{'verh', 'boolean+'}})
argparse({'--verh=42'}, {'niz'})
argparse({'--super-option'})
argparse({'tarantoolctl', 'start', 'instance', '--start=lalochka', 'option', '-', 'another option'})

--
-- gh-4076: argparse incorrectly processed boolean parameters,
-- that led to problems with tarantoolctl usage.
--
params = {}
params[1] = {'flag1', 'boolean'}
params[2] = {'flag2', 'boolean'}
params[3] = {'flag3', 'boolean'}
params[4] = {'flag4', 'boolean'}
params[5] = {'flag5', 'boolean'}
args = {'--flag1', 'true', '--flag2', '1', '--flag3', 'false', '--flag4', '0', '--flag5', 'TrUe'}
argparse(args, params)

args = {'--flag1', 'abc'}
argparse(args, params)

test_run:cmd("clear filter")
