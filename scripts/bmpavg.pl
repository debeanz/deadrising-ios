#!/usr/bin/perl
# Average brightness (0-255) of 24-bit BMP dumps: perl bmpavg.pl file.bmp ...
use strict;
use warnings;

for my $file (@ARGV) {
    open(my $fh, '<:raw', $file) or die "$file: $!";
    local $/;
    my $data = <$fh>;
    close $fh;
    my ($offset) = unpack('V', substr($data, 10, 4));
    my ($width, $height) = unpack('V l', substr($data, 18, 8));
    $height = -$height if $height < 0;
    my $rowBytes = ($width * 3 + 3) & ~3;
    my ($sum, $nonzero) = (0, 0);
    for my $y (0 .. $height - 1) {
        my @row = unpack('C*', substr($data, $offset + $y * $rowBytes, $width * 3));
        for my $v (@row) { $sum += $v; }
        for (my $x = 0; $x < $width; $x++) {
            $nonzero++ if $row[$x * 3] || $row[$x * 3 + 1] || $row[$x * 3 + 2];
        }
    }
    printf "%-70s avg %6.1f  nonzero %5.1f%%\n", $file, $sum / ($width * $height * 3), 100 * $nonzero / ($width * $height);
}
