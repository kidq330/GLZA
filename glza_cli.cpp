#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "glza_api.h"
#include "glza_params.h"
#include "glza_pipeline.h"

namespace {

void print_usage() {
  fprintf(stderr, "ERROR - Invalid format\n");
  fprintf(stderr,
          " Use GLZA c|d [-d0] [-l0] [-m#] [-o#] [-p#] [-r#] [-t1|2] "
          "[-v1|2] [-w0] [-x] [-C0|1] [-D#]\n");
  fprintf(stderr, "   <infile> <outfile>\n");
  fprintf(stderr, " where:\n");
  fprintf(stderr, "   -d0   disables delta transformation\n");
  fprintf(stderr,
          "   -l0   disables capital letter lock transformation\n");
  fprintf(stderr,
          "   -m0|1 overrides the program's decision on whether to use "
          "MTF queues\n");
  fprintf(stderr, "         -m0 disables MTF, -m1 enables MTF\n");
  fprintf(stderr,
          "   -o#   sets the dedupication candidate score order model.  "
          "0.0 is order 0 based, 1.0 is\n");
  fprintf(stderr,
          "         order 1 trailing char/leading char based.  "
          "Intermediate values are a blend.\n");
  fprintf(stderr,
          "   -p#   sets the profit power ratio.  0.0 is most "
          "compressive, larger values favor\n");
  fprintf(stderr, "         longer strings\n");
  fprintf(stderr,
          "   -r#   sets memory usage in millions of bytes\n");
  fprintf(stderr,
          "   -t#   sets the decoder multithreading option. -t1 = 1 "
          "thread, -t2 = 2 threads\n");
  fprintf(stderr,
          "   -v1|2 -v1 causes the dictionary to be printed to stdout, "
          "most frequent first\n");
  fprintf(stderr,
          "         -v2 causes the dictionary to be printed to stdout, "
          "in the order of creation\n");
  fprintf(stderr, "   -x    enables extreme compression mode\n");
  fprintf(stderr,
          "   -w0   disables the initial word deduplication cycle for "
          "files that appear to be text\n");
  fprintf(stderr,
          "   -C0|1 overrides the program's decision on whether to "
          "capital transform\n");
  fprintf(stderr, "         -C0 disables, -C1 enables\n");
  fprintf(stderr,
          "   -D#   sets an upper limit for the number of grammar rules "
          "created\n");
}

}  // namespace

int main(int argc, char* argv[]) {
  glza::Params params;
  params.max_rules = 0xA00000;

  if (argc < 4) {
    print_usage();
    return EXIT_FAILURE;
  }

  const uint8_t mode = static_cast<uint8_t>(*argv[1] - 'c');
  if (mode > 1) {
    fprintf(stderr, "ERROR - mode must be c or d\n");
    return EXIT_FAILURE;
  }

  uint8_t user_set_order = 0;
  int32_t arg_num = 2;

  while (*argv[arg_num] == '-') {
    const char flag = *(argv[arg_num] + 1);
    if (flag == 'C') {
      params.cap_encoded =
          (*(argv[arg_num++] + 2) == '1') ? uint8_t{1} : uint8_t{2};
    } else if (flag == 'D') {
      params.max_rules =
          static_cast<uint32_t>(atoi(argv[arg_num++] + 2));
      if (params.max_rules > 0xC00000) params.max_rules = 0xC00000;
    } else if (flag == 'd') {
      if (*(argv[arg_num] + 2) == '0') params.delta_disabled = 1;
      arg_num++;
    } else if (flag == 'l') {
      if (*(argv[arg_num] + 2) == '0') params.cap_lock_disabled = 1;
      arg_num++;
    } else if (flag == 'm') {
      if (*(argv[arg_num] + 2) == '0')
        params.use_mtf = 0;
      else if (*(argv[arg_num] + 2) == '1')
        params.use_mtf = 1;
      arg_num++;
    } else if (flag == 'o') {
      params.order = atof(argv[arg_num++] + 2);
      user_set_order = 1;
    } else if (flag == 'p') {
      params.profit_ratio_power = atof(argv[arg_num++] + 2);
      params.user_set_profit_ratio_power = 1;
    } else if (flag == 'r') {
      params.user_set_RAM_size = 1;
      params.RAM_usage = atof(argv[arg_num++] + 2);
      if (params.RAM_usage < 60.0) {
        fprintf(stderr, "ERROR: -r value must be >= 60.0 (MB)\n");
        return EXIT_FAILURE;
      }
    } else if (flag == 't') {
      if (*(argv[arg_num++] + 2) != '2') params.two_threads = 0;
    } else if (flag == 'v') {
      if (*(argv[arg_num] + 2) == '1')
        params.print_dictionary = 1;
      else if (*(argv[arg_num] + 2) == '2')
        params.print_dictionary = 2;
      arg_num++;
    } else if (flag == 'w') {
      if (*(argv[arg_num] + 2) == '0') params.create_words = 0;
      arg_num++;
    } else if (flag == 'x') {
      params.fast_mode = 0;
      arg_num++;
    } else {
      fprintf(stderr,
              "ERROR - Invalid format '-%c'.  Only -d<value>, -l0, "
              "-m<value>, -o<value>, -p<value>,\n",
              flag);
      fprintf(stderr,
              "    -r<value>, -t<value>, -v<value>, -w<value> -x "
              "-C<value> and -D<value> allowed.\n");
      return EXIT_FAILURE;
    }
    if (argc < arg_num + 2) {
      print_usage();
      return EXIT_FAILURE;
    }
  }

  if (user_set_order != 0 && params.order != 0.0) params.fast_mode = 0;

  if (argc != arg_num + 2) {
    print_usage();
    return EXIT_FAILURE;
  }

  FILE* fd_in = fopen(argv[arg_num], "rb");
  if (fd_in == nullptr) {
    fprintf(stderr, "ERROR - Unable to open input file '%s'\n",
            argv[arg_num]);
    return EXIT_FAILURE;
  }
  fseeko(fd_in, 0, SEEK_END);
  const auto insize = static_cast<size_t>(ftello(fd_in));
  if (insize > 0xFFFFFFF0) {
    fprintf(stderr, "ERROR - maximum file size is %u bytes\n",
            0xFFFFFFF0u);
    fclose(fd_in);
    return EXIT_FAILURE;
  }
  rewind(fd_in);

  auto* inbuf = static_cast<uint8_t*>(malloc(insize));
  if (inbuf == nullptr) {
    fprintf(stderr, "ERROR - Input buffer memory allocation failed\n");
    fclose(fd_in);
    return EXIT_FAILURE;
  }
  if (fread(inbuf, 1, insize, fd_in) != insize) {
    fprintf(stderr, "ERROR - Read infile failed\n");
    free(inbuf);
    fclose(fd_in);
    return EXIT_FAILURE;
  }
  fclose(fd_in);

  FILE* fd_out = fopen(argv[++arg_num], "wb");
  if (fd_out == nullptr) {
    fprintf(stderr, "ERROR - Unable to open output file '%s'\n",
            argv[arg_num]);
    free(inbuf);
    return EXIT_FAILURE;
  }

  const auto start = std::chrono::steady_clock::now();
  size_t outsize = 0;

  if (mode == 0) {
    if (insize == 0) {
      outsize = 0;
    } else if (!glza::compress(insize, inbuf, &outsize, nullptr, fd_out,
                               params)) {
      fclose(fd_out);
      return EXIT_FAILURE;
    }
    fprintf(stderr, "Compressed %lu bytes -> %lu bytes (%.4f bpB)",
            static_cast<unsigned long>(insize),
            static_cast<unsigned long>(outsize),
            8.0 * static_cast<float>(outsize) /
                static_cast<float>(insize));
  } else {
    uint8_t* outbuf_ptr = nullptr;
    if (insize == 0) {
      outsize = 0;
    } else {
      outbuf_ptr = glza::decompress(insize, inbuf, &outsize, nullptr,
                                    fd_out, params);
      free(inbuf);
      (void)outbuf_ptr;
    }
    fprintf(stderr, "Decompressed %lu bytes -> %lu bytes (%.4f bpB)",
            static_cast<unsigned long>(insize),
            static_cast<unsigned long>(outsize),
            8.0 * static_cast<float>(insize) /
                static_cast<float>(outsize));
  }

  fclose(fd_out);
  const auto end = std::chrono::steady_clock::now();
  const double elapsed =
      std::chrono::duration<double>(end - start).count();
  fprintf(stderr, " in %.3f seconds.\n", elapsed);
  return EXIT_SUCCESS;
}
