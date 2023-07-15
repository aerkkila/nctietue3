# proj-library is used in coordinate transformations in extra/nctproj.
# GNU-extensions must also be available for fopencookie.
# Comment this out if you don't want that.
have_proj = 1

CFLAGS = -Wall -g -fPIC -O3
CC = gcc

prefix = /usr/local
includedir = ${prefix}/include
libdir = ${prefix}/lib
