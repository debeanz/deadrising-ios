#!/usr/bin/perl
# Lists the C files FFmpeg's own Makefiles would build for libavutil and libavcodec, given the
# ffbuild/config.mak that configure wrote. FFmpeg's build needs make, which Windows lacks, so
# scripts/ffmpeg/CMakeLists.txt compiles these instead.
#   perl list_sources.pl <ffmpeg source dir> <configured build dir>  > sources.cmake
use strict; use warnings;
my ($src, $cfgdir) = @ARGV;
my %cfg;
open(my $c, '<', "$cfgdir/ffbuild/config.mak") or die "config.mak: $!";
while (<$c>) { chomp; $cfg{$1} = $2 if /^(!?[A-Za-z0-9_]+)=(.*)$/; }
close $c;

sub expand { my $s = shift; $s =~ s/\$\(([!A-Za-z0-9_]+)\)/defined $cfg{$1} ? $cfg{$1} : ''/ge; return $s; }

sub parse {
    my ($lib, $file, $objs) = @_;
    open(my $f, "<", $file) or die "$file: $!"; binmode $f;
    my $text = do { local $/; <$f> }; close $f;
    $text =~ s/\x5c\r?\n/ /g;
    for my $line (split /\r?\n/, $text) {
        if ($line =~ /^include\s+\$\(SRC_PATH\)\/(\S+)/) { my $inc = "$src/$1"; parse($lib, $inc, $objs) if -f $inc && $inc =~ /\/Makefile$/; next; }
        next unless $line =~ /^(OBJS(?:-\S+)?)\s*(\+?=)\s*(.*)$/;
        my ($var, $list) = (expand($1), $3);
        next unless $var eq 'OBJS' || $var eq 'OBJS-yes';
        $list = expand($list);
        push @$objs, map { "$lib/$_" } grep { /\.o$/ } split /\s+/, $list;
    }
}

for my $lib ('libavutil', 'libavcodec') {
    my @objs; parse($lib, "$src/$lib/Makefile", \@objs);
    my (%seen, @files);
    for my $o (@objs) {
        (my $base = $o) =~ s/\.o$//;
        # Generated list files live in the build directory.
        my $path = -f "$cfgdir/$base.c" ? "$cfgdir/$base.c" : "$src/$base.c";
        $path =~ s{/[^/]+/\.\./}{/}g;
        next if $seen{$path}++;
        warn "missing $path\n" unless -f $path;
        push @files, $path if -f $path;
    }
    my $name = uc($lib); $name =~ s/^LIB//;
    print "set(FF_${name}_SOURCES\n", (map { "    \"$_\"\n" } @files), ")\n";
}
