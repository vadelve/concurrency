# Build instructions

In the directory with source code root run the following:
```
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ../
make
```

If you want to choose compilers run cmake like this:
```
CXX=/usr/bin/g++-11 CC=/usr/bin/gcc-11 cmake -DCMAKE_BUILD_TYPE=Release ../
```

# Running benchmarks and tests
```
cd build
bin/circular_buffer_test
```
