require_relative 'mrblib/jsonsl/version'

MRuby::Gem::Specification.new('mruby-jsonsl') do |spec|
  spec.license = 'MIT'
  spec.author  = 'Team Yamanekko'
  spec.version = JSONSL::VERSION
  spec.summary = 'mruby binding to JSONSL parser library'
  spec.homepage = 'https://github.com/yamanekko/mruby-jsonsl'
end
