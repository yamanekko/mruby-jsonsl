assert('JSONSL.new') do
  assert_equal(JSONSL, JSONSL.new.class)
end
assert('JSONSL.parse') do
  assert_equal(true, JSONSL.new.parse('{"foo":true}'))
end
