#!/usr/bin/perl
# If you think this is stupid/overkill, blame dormando

use warnings;
use strict;

my $version = `git describe`;
chomp $version;
# Test the various versions.
#my $version = 'foob';
#my $version = '1.4.2-30-gf966dba';
#my $version = '1.4.3-rc1';
#my $version = '1.4.3';
#my $version = 'membase-1.7.1.1-44-gc0d22b8';
unless ($version =~ m/\d+\.\d+\.\d+(.\d+)?/) {
    write_file('m4/version.m4', "m4_define([VERSION_NUMBER], [UNKNOWN])\n");
    exit;
}

$version =~ s/^[a-z]+-//g;
$version =~ s/-/_/g;
write_file('m4/version.m4', "m4_define([VERSION_NUMBER], [$version])\n");

my ($VERSION, $FULLVERSION, $RELEASE);

if ($version =~ m/^(\d+\.\d+\.\d+)_rc(\d+)$/) {
    $VERSION = $1;
    $FULLVERSION = $version;
    $RELEASE = '0.1.rc' . $2;
} elsif ($version =~ m/^(\d+\.\d+\.\d+(.\d+)?)_(.+)$/) {
    $VERSION = $1;
    $FULLVERSION = $version;
    $RELEASE = '1.' . $3;
} elsif ($version =~ m/^(\d+\.\d+\.\d+)$/) {
    $VERSION = $1;
    $FULLVERSION = $version;
    $RELEASE = '1';
}

my $spec = read_file('memcached.spec.in');
$spec =~ s/\@VERSION\@/$VERSION/gm;
$spec =~ s/\@FULLVERSION\@/$FULLVERSION/gm;
$spec =~ s/\@RELEASE\@/$RELEASE/gm;

write_file('memcached.spec', $spec);

sub write_file {
    my $file = shift;
    my $data = shift;
    open(my $fh, "> $file") or die "Can't open $file: $!";
    print $fh $data;
    close($fh);
}

sub read_file {
    my $file = shift;
    local $/ = undef;
    open(my $fh, "< $file") or die "Can't open $file: $!";
    my $data = <$fh>;
    close($fh);
    return $data;
}
