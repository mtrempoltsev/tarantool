remote = require('net.box')
test_run = require('test_run').new()

--
-- gh-4511: make sure that SET works.
--
box.execute('SELECT "name" FROM "_vsession_settings";')

engine = box.space._vsession_settings:get{'sql_default_engine'}.value
order = box.space._vsession_settings:get{'sql_reverse_unordered_selects'}.value

box.execute('SET sql_default_engine = 1;')
box.execute("SET sql_default_engine = 'some_engine';")
box.execute("SET engine = 'vinyl';")
box.execute("SET sql_defer_foreign_keys = 'vinyl';")
engine == box.space._vsession_settings:get{'sql_default_engine'}.value
order == box.space._vsession_settings:get{'sql_reverse_unordered_selects'}.value

box.execute("SET sql_default_engine = 'vinyl';")
box.execute("SET sql_reverse_unordered_selects = true;")
box.execute('SELECT * FROM "_vsession_settings";')

box.execute("SET sql_default_engine = 'memtx';")
box.execute("SET sql_reverse_unordered_selects = false;")
box.execute('SELECT * FROM "_vsession_settings";')

box.execute("SET sql_default_engine = '"..engine.."';")
box.execute("SET sql_reverse_unordered_selects = "..tostring(order)..";")
