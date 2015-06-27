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

assert('JSONSL#parse_uescape1') do
  assert_equal({"foo"=>"barJC"}, JSONSL.new.parse('{"foo":"bar\\u004aC"}'))
end
assert('JSONSL#parse_uescape2') do
  assert_equal({"foo"=>"テスト"}, JSONSL.new.parse('{"foo":"\\u30C6\\u30B9\\u30C8"}'))
end
assert('JSONSL#parse_uescape2b') do
  assert_equal({"foo"=>"abcテスト_!"}, JSONSL.new.parse('{"foo":"abc\\u30C6\\u30B9\\u30C8_!"}'))
end
assert('JSONSL#parse_uescape_surrogate_pair') do
  assert_equal({"foo"=>"abc\xED\xA0\xB4\xED\xB4\x9E"}, JSONSL.new.parse('{"foo":"abc\\uD834\\uDD1E"}'))
end
assert('JSONSL#parse_uescape_error') do
  assert_raise(JSONSL::Error) do
    JSONSL.new.parse('{"foo":"\u30g6"}')
  end
end
assert('JSONSL#parse_uescape_error2') do
  assert_raise(JSONSL::Error) do
    JSONSL.new.parse('{"foo":"\u30_6"}')
  end
end
assert('JSONSL#parse_uescape_error3') do
  assert_raise(JSONSL::Error) do
    JSONSL.new.parse('{"foo":"abc\u306"}')
  end
end
assert('JSONSL#parse_uescape_error4') do
  assert_raise(JSONSL::Error) do
    JSONSL.new.parse('{"foo":"abc\!"}')
  end
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
assert('JSONSL::Error') do
  assert_raise(JSONSL::Error) do
    json = JSONSL.parse('{"foo":nil}')
  end
end
assert('JSONSL::Error') do
  assert_raise(JSONSL::Error) do
    json = JSONSL.parse('{"foo":')
  end
end
assert('JSONSL::Error') do
  assert_raise(JSONSL::Error) do
    json = JSONSL.parse('{"foo":true}}')
  end
end
assert('too much nested JSON') do
  str = ("["*10000) + "0" + ("]"*10000)
  json = JSONSL.new(10002).parse(str)
end
assert('too much nested JSON and copy') do
  str = ("["*10000) + "0" + ("]"*10000)
  json = JSONSL.new(10002)
  json2 = json.dup
  json2.parse(str)
end
