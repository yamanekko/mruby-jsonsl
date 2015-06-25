# mruby-jsonsl

mruby-jsonsl is a mruby binding of jsonsl(https://github.com/mnunberg/jsonsl), an Embeddable, Fast, Streaming, Non-Buffering JSON Parser.

## Usage

```ruby
parser = JSONSL.new
json = parser.parse("[1,2,true,null,{\"foo\":\"bar\"}]")

## or

json = JSONSL.parse("[1,2,true,null,{\"foo\":\"bar\"}]")
```

## Install

Add conf. in build_config.rb.

```ruby
  conf.gem github: 'json/mruby-jsonsl'
```

and build with rake.

```
$ rake
```

## Author

Team Yamanekko (@yamanekko)

