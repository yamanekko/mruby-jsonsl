assert('JSONSL.new') do
  assert_equal(JSONSL, JSONSL.new.class)
end
assert('JSONSL#parse null array') do
  assert_equal([], JSONSL.new.parse('[]'))
end
assert('JSONSL#parse null hash') do
  assert_equal({}, JSONSL.new.parse('{}'))
end
assert('JSONSL#parse') do
  assert_equal({"foo"=>true}, JSONSL.new.parse('{"foo":true}'))
end
assert('JSONSL#parse2') do
  assert_equal({"foo"=>["bar","buz",false,nil,"hoge",{"a"=>"b"}]}, JSONSL.new.parse('{"foo":["bar","buz",false,null,"hoge",{"a":"b"}]}'))
end
assert('JSONSL#parse3') do
  assert_equal({"foo"=>[1,2,3.14,"hoge",{"a"=>"b"}]}, JSONSL.new.parse('{"foo":[1,2,3.14,"hoge",{"a":"b"}]}'))
end
assert('JSONSL.parse') do
  assert_equal({"foo"=>true}, JSONSL.parse('{"foo":true}'))
end
assert('JSONSL.parse.hash') do
  assert_equal({:foo=>true}, JSONSL.new.parse('{"foo":true}',{:symbol_key => true}))
end
assert('JSONSL#dup') do
  json = JSONSL.new.dup.parse('{"foo":[1,2,3.14,"hoge",{"a":"b"}]}')
  assert_equal(json, json.dup)
end
