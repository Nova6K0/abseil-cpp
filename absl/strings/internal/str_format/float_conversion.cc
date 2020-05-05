#include "absl/strings/internal/str_format/float_conversion.h"

#include <string.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <string>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/internal/bits.h"
#include "absl/base/optimization.h"
#include "absl/functional/function_ref.h"
#include "absl/meta/type_traits.h"
#include "absl/numeric/int128.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace str_format_internal {

namespace {

// The code below wants to avoid heap allocations.
// To do so it needs to allocate memory on the stack.
// `StackArray` will allocate memory on the stack in the form of a uint32_t
// array and call the provided callback with said memory.
// It will allocate memory in increments of 512 bytes. We could allocate the
// largest needed unconditionally, but that is more than we need in most of
// cases. This way we use less stack in the common cases.
class StackArray {
  using Func = absl::FunctionRef<void(absl::Span<uint32_t>)>;
  static constexpr size_t kStep = 512 / sizeof(uint32_t);
  // 5 steps is 2560 bytes, which is enough to hold a long double with the
  // largest/smallest exponents.
  // The operations below will static_assert their particular maximum.
  static constexpr size_t kNumSteps = 5;

  // We do not want this function to be inlined.
  // Otherwise the caller will allocate the stack space unnecessarily for all
  // the variants even though it only calls one.
  template <size_t steps>
  ABSL_ATTRIBUTE_NOINLINE static void RunWithCapacityImpl(Func f) {
    uint32_t values[steps * kStep]{};
    f(absl::MakeSpan(values));
  }

 public:
  static constexpr size_t kMaxCapacity = kStep * kNumSteps;

  static void RunWithCapacity(size_t capacity, Func f) {
    assert(capacity <= kMaxCapacity);
    const size_t step = (capacity + kStep - 1) / kStep;
    assert(step <= kNumSteps);
    switch (step) {
      case 1:
        return RunWithCapacityImpl<1>(f);
      case 2:
        return RunWithCapacityImpl<2>(f);
      case 3:
        return RunWithCapacityImpl<3>(f);
      case 4:
        return RunWithCapacityImpl<4>(f);
      case 5:
        return RunWithCapacityImpl<5>(f);
    }

    assert(false && "Invalid capacity");
  }
};

// Calculates `10 * (*v) + carry` and stores the result in `*v` and returns
// the carry.
template <typename Int>
inline Int MultiplyBy10WithCarry(Int *v, Int carry) {
  using BiggerInt = absl::conditional_t<sizeof(Int) == 4, uint64_t, uint128>;
  BiggerInt tmp = 10 * static_cast<BiggerInt>(*v) + carry;
  *v = static_cast<Int>(tmp);
  return static_cast<Int>(tmp >> (sizeof(Int) * 8));
}

// Calculates `(2^64 * carry + *v) / 10`.
// Stores the quotient in `*v` and returns the remainder.
// Requires: `0 <= carry <= 9`
inline uint64_t DivideBy10WithCarry(uint64_t *v, uint64_t carry) {
  constexpr uint64_t divisor = 10;
  // 2^64 / divisor = chunk_quotient + chunk_remainder / divisor
  constexpr uint64_t chunk_quotient = (uint64_t{1} << 63) / (divisor / 2);
  constexpr uint64_t chunk_remainder = uint64_t{} - chunk_quotient * divisor;

  const uint64_t mod = *v % divisor;
  const uint64_t next_carry = chunk_remainder * carry + mod;
  *v = *v / divisor + carry * chunk_quotient + next_carry / divisor;
  return next_carry % divisor;
}

// Generates the decimal representation for an integer of the form `v * 2^exp`,
// where `v` and `exp` are both positive integers.
// It generates the digits from the left (ie the most significant digit first)
// to allow for direct printing into the sink.
//
// Requires `0 <= exp` and `exp <= numeric_limits<long double>::max_exponent`.
class BinaryToDecimal {
  static constexpr int ChunksNeeded(int exp) {
    // We will left shift a uint128 by `exp` bits, so we need `128+exp` total
    // bits. Round up to 32.
    // See constructor for details about adding `10%` to the value.
    return (128 + exp + 31) / 32 * 11 / 10;
  }

 public:
  // Run the conversion for `v * 2^exp` and call `f(binary_to_decimal)`.
  // This function will allocate enough stack space to perform the conversion.
  static void RunConversion(uint128 v, int exp,
                            absl::FunctionRef<void(BinaryToDecimal)> f) {
    assert(exp > 0);
    assert(exp <= std::numeric_limits<long double>::max_exponent);
    static_assert(
        StackArray::kMaxCapacity >=
            ChunksNeeded(std::numeric_limits<long double>::max_exponent),
        "");

    StackArray::RunWithCapacity(
        ChunksNeeded(exp),
        [=](absl::Span<uint32_t> input) { f(BinaryToDecimal(input, v, exp)); });
  }

  int TotalDigits() const {
    return static_cast<int>((decimal_end_ - decimal_start_) * kDigitsPerChunk +
                            CurrentDigits().size());
  }

  // See the current block of digits.
  absl::string_view CurrentDigits() const {
    return absl::string_view(digits_ + kDigitsPerChunk - size_, size_);
  }

  // Advance the current view of digits.
  // Returns `false` when no more digits are available.
  bool AdvanceDigits() {
    if (decimal_start_ >= decimal_end_) return false;

    uint32_t w = data_[decimal_start_++];
    for (size_ = 0; size_ < kDigitsPerChunk; w /= 10) {
      digits_[kDigitsPerChunk - ++size_] = w % 10 + '0';
    }
    return true;
  }

 private:
  BinaryToDecimal(absl::Span<uint32_t> data, uint128 v, int exp) : data_(data) {
    // We need to print the digits directly into the sink object without
    // buffering them all first. To do this we need two things:
    // - to know the total number of digits to do padding when necessary
    // - to generate the decimal digits from the left.
    //
    // In order to do this, we do a two pass conversion.
    // On the first pass we convert the binary representation of the value into
    // a decimal representation in which each uint32_t chunk holds up to 9
    // decimal digits.  In the second pass we take each decimal-holding-uint32_t
    // value and generate the ascii decimal digits into `digits_`.
    //
    // The binary and decimal representations actually share the same memory
    // region. As we go converting the chunks from binary to decimal we free
    // them up and reuse them for the decimal representation. One caveat is that
    // the decimal representation is around 7% less efficient in space than the
    // binary one. We allocate an extra 10% memory to account for this. See
    // ChunksNeeded for this calculation.
    int chunk_index = exp / 32;
    decimal_start_ = decimal_end_ = ChunksNeeded(exp);
    const int offset = exp % 32;
    // Left shift v by exp bits.
    data_[chunk_index] = static_cast<uint32_t>(v << offset);
    for (v >>= (32 - offset); v; v >>= 32)
      data_[++chunk_index] = static_cast<uint32_t>(v);

    while (chunk_index >= 0) {
      // While we have more than one chunk available, go in steps of 1e9.
      // `data_[chunk_index]` holds the highest non-zero binary chunk, so keep
      // the variable updated.
      uint32_t carry = 0;
      for (int i = chunk_index; i >= 0; --i) {
        uint64_t tmp = uint64_t{data_[i]} + (uint64_t{carry} << 32);
        data_[i] = static_cast<uint32_t>(tmp / uint64_t{1000000000});
        carry = static_cast<uint32_t>(tmp % uint64_t{1000000000});
      }

      // If the highest chunk is now empty, remove it from view.
      if (data_[chunk_index] == 0) --chunk_index;

      --decimal_start_;
      assert(decimal_start_ != chunk_index);
      data_[decimal_start_] = carry;
    }

    // Fill the first set of digits. The first chunk might not be complete, so
    // handle differently.
    for (uint32_t first = data_[decimal_start_++]; first != 0; first /= 10) {
      digits_[kDigitsPerChunk - ++size_] = first % 10 + '0';
    }
  }

 private:
  static constexpr size_t kDigitsPerChunk = 9;

  int decimal_start_;
  int decimal_end_;

  char digits_[kDigitsPerChunk];
  int size_ = 0;

  absl::Span<uint32_t> data_;
};

// Converts a value of the form `x * 2^-exp` into a sequence of decimal digits.
// Requires `-exp < 0` and
// `-exp >= limits<long double>::min_exponent - limits<long double>::digits`.
class FractionalDigitGenerator {
 public:
  // Run the conversion for `v * 2^exp` and call `f(generator)`.
  // This function will allocate enough stack space to perform the conversion.
  static void RunConversion(
      uint128 v, int exp, absl::FunctionRef<void(FractionalDigitGenerator)> f) {
    assert(-exp < 0);
    assert(-exp >= std::numeric_limits<long double>::min_exponent - 128);
    static_assert(
        StackArray::kMaxCapacity >=
            (128 - std::numeric_limits<long double>::min_exponent + 31) / 32,
        "");
    StackArray::RunWithCapacity((exp + 31) / 32,
                                [=](absl::Span<uint32_t> input) {
                                  f(FractionalDigitGenerator(input, v, exp));
                                });
  }

  // Returns true if there are any more non-zero digits left.
  bool HasMoreDigits() const { return next_digit_ != 0 || chunk_index_ >= 0; }

  // Returns true if the remainder digits are greater than 5000...
  bool IsGreaterThanHalf() const {
    return next_digit_ > 5 || (next_digit_ == 5 && chunk_index_ >= 0);
  }
  // Returns true if the remainder digits are exactly 5000...
  bool IsExactlyHalf() const { return next_digit_ == 5 && chunk_index_ < 0; }

  struct Digits {
    int digit_before_nine;
    int num_nines;
  };

  // Get the next set of digits.
  // They are composed by a non-9 digit followed by a runs of zero or more 9s.
  Digits GetDigits() {
    Digits digits{next_digit_, 0};

    next_digit_ = GetOneDigit();
    while (next_digit_ == 9) {
      ++digits.num_nines;
      next_digit_ = GetOneDigit();
    }

    return digits;
  }

 private:
  // Return the next digit.
  int GetOneDigit() {
    if (chunk_index_ < 0) return 0;

    uint32_t carry = 0;
    for (int i = chunk_index_; i >= 0; --i) {
      carry = MultiplyBy10WithCarry(&data_[i], carry);
    }
    // If the lowest chunk is now empty, remove it from view.
    if (data_[chunk_index_] == 0) --chunk_index_;
    return carry;
  }

  FractionalDigitGenerator(absl::Span<uint32_t> data, uint128 v, int exp)
      : chunk_index_(exp / 32), data_(data) {
    const int offset = exp % 32;
    // Right shift `v` by `exp` bits.
    data_[chunk_index_] = static_cast<uint32_t>(v << (32 - offset));
    v >>= offset;
    // Make sure we don't overflow the data. We already calculated that
    // non-zero bits fit, so we might not have space for leading zero bits.
    for (int pos = chunk_index_; v; v >>= 32)
      data_[--pos] = static_cast<uint32_t>(v);

    // Fill next_digit_, as GetDigits expects it to be populated always.
    next_digit_ = GetOneDigit();
  }

  int next_digit_;
  int chunk_index_;
  absl::Span<uint32_t> data_;
};

// Count the number of leading zero bits.
int LeadingZeros(uint64_t v) { return base_internal::CountLeadingZeros64(v); }
int LeadingZeros(uint128 v) {
  auto high = static_cast<uint64_t>(v >> 64);
  auto low = static_cast<uint64_t>(v);
  return high != 0 ? base_internal::CountLeadingZeros64(high)
                   : 64 + base_internal::CountLeadingZeros64(low);
}

// Round up the text digits starting at `p`.
// The buffer must have an extra digit that is known to not need rounding.
// This is done below by having an extra '0' digit on the left.
void RoundUp(char *p) {
  while (*p == '9' || *p == '.') {
    if (*p == '9') *p = '0';
    --p;
  }
  ++*p;
}

// Check the previous digit and round up or down to follow the round-to-even
// policy.
void RoundToEven(char *p) {
  if (*p == '.') --p;
  if (*p % 2 == 1) RoundUp(p);
}

// Simple integral decimal digit printing for values that fit in 64-bits.
// Returns the pointer to the last written digit.
char *PrintIntegralDigitsFromRightFast(uint64_t v, char *p) {
  do {
    *--p = DivideBy10WithCarry(&v, 0) + '0';
  } while (v != 0);
  return p;
}

// Simple integral decimal digit printing for values that fit in 128-bits.
// Returns the pointer to the last written digit.
char *PrintIntegralDigitsFromRightFast(uint128 v, char *p) {
  auto high = static_cast<uint64_t>(v >> 64);
  auto low = static_cast<uint64_t>(v);

  while (high != 0) {
    uint64_t carry = DivideBy10WithCarry(&high, 0);
    carry = DivideBy10WithCarry(&low, carry);
    *--p = carry + '0';
  }
  return PrintIntegralDigitsFromRightFast(low, p);
}

// Simple fractional decimal digit printing for values that fir in 64-bits after
// shifting.
// Performs rounding if necessary to fit within `precision`.
// Returns the pointer to one after the last character written.
char *PrintFractionalDigitsFast(uint64_t v, char *start, int exp,
                                int precision) {
  char *p = start;
  v <<= (64 - exp);
  while (precision > 0) {
    if (!v) return p;
    *p++ = MultiplyBy10WithCarry(&v, uint64_t{0}) + '0';
    --precision;
  }

  // We need to round.
  if (v < 0x8000000000000000) {
    // We round down, so nothing to do.
  } else if (v > 0x8000000000000000) {
    // We round up.
    RoundUp(p - 1);
  } else {
    RoundToEven(p - 1);
  }

  assert(precision == 0);
  // Precision can only be zero here.
  return p;
}

// Simple fractional decimal digit printing for values that fir in 128-bits
// after shifting.
// Performs rounding if necessary to fit within `precision`.
// Returns the pointer to one after the last character written.
char *PrintFractionalDigitsFast(uint128 v, char *start, int exp,
                                int precision) {
  char *p = start;
  v <<= (128 - exp);
  auto high = static_cast<uint64_t>(v >> 64);
  auto low = static_cast<uint64_t>(v);

  // While we have digits to print and `low` is not empty, do the long
  // multiplication.
  while (precision > 0 && low != 0) {
    uint64_t carry = MultiplyBy10WithCarry(&low, uint64_t{0});
    carry = MultiplyBy10WithCarry(&high, carry);

    *p++ = carry + '0';
    --precision;
  }

  // Now `low` is empty, so use a faster approach for the rest of the digits.
  // This block is pretty much the same as the main loop for the 64-bit case
  // above.
  while (precision > 0) {
    if (!high) return p;
    *p++ = MultiplyBy10WithCarry(&high, uint64_t{0}) + '0';
    --precision;
  }

  // We need to round.
  if (high < 0x8000000000000000) {
    // We round down, so nothing to do.
  } else if (high > 0x8000000000000000 || low != 0) {
    // We round up.
    RoundUp(p - 1);
  } else {
    RoundToEven(p - 1);
  }

  assert(precision == 0);
  // Precision can only be zero here.
  return p;
}

struct FormatState {
  char sign_char;
  int precision;
  const FormatConversionSpecImpl &conv;
  FormatSinkImpl *sink;

  // In `alt` mode (flag #) we keep the `.` even if there are no fractional
  // digits. In non-alt mode, we strip it.
  bool ShouldPrintDot() const { return precision != 0 || conv.has_alt_flag(); }
};

struct Padding {
  int left_spaces;
  int zeros;
  int right_spaces;
};

Padding ExtraWidthToPadding(int total_size, const FormatState &state) {
  int missing_chars = std::max(state.conv.width() - total_size, 0);
  if (state.conv.has_left_flag()) {
    return {0, 0, missing_chars};
  } else if (state.conv.has_zero_flag()) {
    return {0, missing_chars, 0};
  } else {
    return {missing_chars, 0, 0};
  }
}

void FinalPrint(absl::string_view data, int trailing_zeros,
                const FormatState &state) {
  if (state.conv.width() < 0) {
    // No width specified. Fast-path.
    if (state.sign_char != '\0') state.sink->Append(1, state.sign_char);
    state.sink->Append(data);
    state.sink->Append(trailing_zeros, '0');
    return;
  }

  auto padding =
      ExtraWidthToPadding((state.sign_char != '\0' ? 1 : 0) +
                              static_cast<int>(data.size()) + trailing_zeros,
                          state);

  state.sink->Append(padding.left_spaces, ' ');
  if (state.sign_char != '\0') state.sink->Append(1, state.sign_char);
  state.sink->Append(padding.zeros, '0');
  state.sink->Append(data);
  state.sink->Append(trailing_zeros, '0');
  state.sink->Append(padding.right_spaces, ' ');
}

// Fastpath %f formatter for when the shifted value fits in a simple integral
// type.
// Prints `v*2^exp` with the options from `state`.
template <typename Int>
void FormatFFast(Int v, int exp, const FormatState &state) {
  constexpr int input_bits = sizeof(Int) * 8;

  static constexpr size_t integral_size =
      /* in case we need to round up an extra digit */ 1 +
      /* decimal digits for uint128 */ 40 + 1;
  char buffer[integral_size + /* . */ 1 + /* max digits uint128 */ 128];
  buffer[integral_size] = '.';
  char *const integral_digits_end = buffer + integral_size;
  char *integral_digits_start;
  char *const fractional_digits_start = buffer + integral_size + 1;
  char *fractional_digits_end = fractional_digits_start;

  if (exp >= 0) {
    const int total_bits = input_bits - LeadingZeros(v) + exp;
    integral_digits_start =
        total_bits <= 64
            ? PrintIntegralDigitsFromRightFast(static_cast<uint64_t>(v) << exp,
                                               integral_digits_end)
            : PrintIntegralDigitsFromRightFast(static_cast<uint128>(v) << exp,
                                               integral_digits_end);
  } else {
    exp = -exp;

    integral_digits_start = PrintIntegralDigitsFromRightFast(
        exp < input_bits ? v >> exp : 0, integral_digits_end);
    // PrintFractionalDigits may pull a carried 1 all the way up through the
    // integral portion.
    integral_digits_start[-1] = '0';

    fractional_digits_end =
        exp <= 64 ? PrintFractionalDigitsFast(v, fractional_digits_start, exp,
                                              state.precision)
                  : PrintFractionalDigitsFast(static_cast<uint128>(v),
                                              fractional_digits_start, exp,
                                              state.precision);
    // There was a carry, so include the first digit too.
    if (integral_digits_start[-1] != '0') --integral_digits_start;
  }

  size_t size = fractional_digits_end - integral_digits_start;

  // In `alt` mode (flag #) we keep the `.` even if there are no fractional
  // digits. In non-alt mode, we strip it.
  if (!state.ShouldPrintDot()) --size;
  FinalPrint(absl::string_view(integral_digits_start, size),
             static_cast<int>(state.precision - (fractional_digits_end -
                                                 fractional_digits_start)),
             state);
}

// Slow %f formatter for when the shifted value does not fit in a uint128, and
// `exp > 0`.
// Prints `v*2^exp` with the options from `state`.
// This one is guaranteed to not have fractional digits, so we don't have to
// worry about anything after the `.`.
void FormatFPositiveExpSlow(uint128 v, int exp, const FormatState &state) {
  BinaryToDecimal::RunConversion(v, exp, [&](BinaryToDecimal btd) {
    const int total_digits =
        btd.TotalDigits() + (state.ShouldPrintDot() ? state.precision + 1 : 0);

    const auto padding = ExtraWidthToPadding(
        total_digits + (state.sign_char != '\0' ? 1 : 0), state);

    state.sink->Append(padding.left_spaces, ' ');
    if (state.sign_char != '\0') state.sink->Append(1, state.sign_char);
    state.sink->Append(padding.zeros, '0');

    do {
      state.sink->Append(btd.CurrentDigits());
    } while (btd.AdvanceDigits());

    if (state.ShouldPrintDot()) state.sink->Append(1, '.');
    state.sink->Append(state.precision, '0');
    state.sink->Append(padding.right_spaces, ' ');
  });
}

// Slow %f formatter for when the shifted value does not fit in a uint128, and
// `exp < 0`.
// Prints `v*2^exp` with the options from `state`.
// This one is guaranteed to be < 1.0, so we don't have to worry about integral
// digits.
void FormatFNegativeExpSlow(uint128 v, int exp, const FormatState &state) {
  const int total_digits =
      /* 0 */ 1 + (state.ShouldPrintDot() ? state.precision + 1 : 0);
  auto padding =
      ExtraWidthToPadding(total_digits + (state.sign_char ? 1 : 0), state);
  padding.zeros += 1;
  state.sink->Append(padding.left_spaces, ' ');
  if (state.sign_char != '\0') state.sink->Append(1, state.sign_char);
  state.sink->Append(padding.zeros, '0');

  if (state.ShouldPrintDot()) state.sink->Append(1, '.');

  // Print digits
  int digits_to_go = state.precision;

  FractionalDigitGenerator::RunConversion(
      v, exp, [&](FractionalDigitGenerator digit_gen) {
        // There are no digits to print here.
        if (state.precision == 0) return;

        // We go one digit at a time, while keeping track of runs of nines.
        // The runs of nines are used to perform rounding when necessary.

        while (digits_to_go > 0 && digit_gen.HasMoreDigits()) {
          auto digits = digit_gen.GetDigits();

          // Now we have a digit and a run of nines.
          // See if we can print them all.
          if (digits.num_nines + 1 < digits_to_go) {
            // We don't have to round yet, so print them.
            state.sink->Append(1, digits.digit_before_nine + '0');
            state.sink->Append(digits.num_nines, '9');
            digits_to_go -= digits.num_nines + 1;

          } else {
            // We can't print all the nines, see where we have to truncate.

            bool round_up = false;
            if (digits.num_nines + 1 > digits_to_go) {
              // We round up at a nine. No need to print them.
              round_up = true;
            } else {
              // We can fit all the nines, but truncate just after it.
              if (digit_gen.IsGreaterThanHalf()) {
                round_up = true;
              } else if (digit_gen.IsExactlyHalf()) {
                // Round to even
                round_up =
                    digits.num_nines != 0 || digits.digit_before_nine % 2 == 1;
              }
            }

            if (round_up) {
              state.sink->Append(1, digits.digit_before_nine + '1');
              --digits_to_go;
              // The rest will be zeros.
            } else {
              state.sink->Append(1, digits.digit_before_nine + '0');
              state.sink->Append(digits_to_go - 1, '9');
              digits_to_go = 0;
            }
            return;
          }
        }
      });

  state.sink->Append(digits_to_go, '0');
  state.sink->Append(padding.right_spaces, ' ');
}

template <typename Int>
void FormatF(Int mantissa, int exp, const FormatState &state) {
  if (exp >= 0) {
    const int total_bits = sizeof(Int) * 8 - LeadingZeros(mantissa) + exp;

    // Fallback to the slow stack-based approach if we can't do it in a 64 or
    // 128 bit state.
    if (ABSL_PREDICT_FALSE(total_bits > 128)) {
      return FormatFPositiveExpSlow(mantissa, exp, state);
    }
  } else {
    // Fallback to the slow stack-based approach if we can't do it in a 64 or
    // 128 bit state.
    if (ABSL_PREDICT_FALSE(exp < -128)) {
      return FormatFNegativeExpSlow(mantissa, -exp, state);
    }
  }
  return FormatFFast(mantissa, exp, state);
}

char *CopyStringTo(absl::string_view v, char *out) {
  std::memcpy(out, v.data(), v.size());
  return out + v.size();
}

template <typename Float>
bool FallbackToSnprintf(const Float v, const FormatConversionSpecImpl &conv,
                        FormatSinkImpl *sink) {
  int w = conv.width() >= 0 ? conv.width() : 0;
  int p = conv.precision() >= 0 ? conv.precision() : -1;
  char fmt[32];
  {
    char *fp = fmt;
    *fp++ = '%';
    fp = CopyStringTo(FormatConversionSpecImplFriend::FlagsToString(conv), fp);
    fp = CopyStringTo("*.*", fp);
    if (std::is_same<long double, Float>()) {
      *fp++ = 'L';
    }
    *fp++ = FormatConversionCharToChar(conv.conversion_char());
    *fp = 0;
    assert(fp < fmt + sizeof(fmt));
  }
  std::string space(512, '\0');
  absl::string_view result;
  while (true) {
    int n = snprintf(&space[0], space.size(), fmt, w, p, v);
    if (n < 0) return false;
    if (static_cast<size_t>(n) < space.size()) {
      result = absl::string_view(space.data(), n);
      break;
    }
    space.resize(n + 1);
  }
  sink->Append(result);
  return true;
}

// 128-bits in decimal: ceil(128*log(2)/log(10))
//   or std::numeric_limits<__uint128_t>::digits10
constexpr int kMaxFixedPrecision = 39;

constexpr int kBufferLength = /*sign*/ 1 +
                              /*integer*/ kMaxFixedPrecision +
                              /*point*/ 1 +
                              /*fraction*/ kMaxFixedPrecision +
                              /*exponent e+123*/ 5;

struct Buffer {
  void push_front(char c) {
    assert(begin > data);
    *--begin = c;
  }
  void push_back(char c) {
    assert(end < data + sizeof(data));
    *end++ = c;
  }
  void pop_back() {
    assert(begin < end);
    --end;
  }

  char &back() {
    assert(begin < end);
    return end[-1];
  }

  char last_digit() const { return end[-1] == '.' ? end[-2] : end[-1]; }

  int size() const { return static_cast<int>(end - begin); }

  char data[kBufferLength];
  char *begin;
  char *end;
};

enum class FormatStyle { Fixed, Precision };

// If the value is Inf or Nan, print it and return true.
// Otherwise, return false.
template <typename Float>
bool ConvertNonNumericFloats(char sign_char, Float v,
                             const FormatConversionSpecImpl &conv,
                             FormatSinkImpl *sink) {
  char text[4], *ptr = text;
  if (sign_char != '\0') *ptr++ = sign_char;
  if (std::isnan(v)) {
    ptr = std::copy_n(
        FormatConversionCharIsUpper(conv.conversion_char()) ? "NAN" : "nan", 3,
        ptr);
  } else if (std::isinf(v)) {
    ptr = std::copy_n(
        FormatConversionCharIsUpper(conv.conversion_char()) ? "INF" : "inf", 3,
        ptr);
  } else {
    return false;
  }

  return sink->PutPaddedString(string_view(text, ptr - text), conv.width(), -1,
                               conv.has_left_flag());
}

// Round up the last digit of the value.
// It will carry over and potentially overflow. 'exp' will be adjusted in that
// case.
template <FormatStyle mode>
void RoundUp(Buffer *buffer, int *exp) {
  char *p = &buffer->back();
  while (p >= buffer->begin && (*p == '9' || *p == '.')) {
    if (*p == '9') *p = '0';
    --p;
  }

  if (p < buffer->begin) {
    *p = '1';
    buffer->begin = p;
    if (mode == FormatStyle::Precision) {
      std::swap(p[1], p[2]);  // move the .
      ++*exp;
      buffer->pop_back();
    }
  } else {
    ++*p;
  }
}

void PrintExponent(int exp, char e, Buffer *out) {
  out->push_back(e);
  if (exp < 0) {
    out->push_back('-');
    exp = -exp;
  } else {
    out->push_back('+');
  }
  // Exponent digits.
  if (exp > 99) {
    out->push_back(exp / 100 + '0');
    out->push_back(exp / 10 % 10 + '0');
    out->push_back(exp % 10 + '0');
  } else {
    out->push_back(exp / 10 + '0');
    out->push_back(exp % 10 + '0');
  }
}

template <typename Float, typename Int>
constexpr bool CanFitMantissa() {
  return
#if defined(__clang__) && !defined(__SSE3__)
      // Workaround for clang bug: https://bugs.llvm.org/show_bug.cgi?id=38289
      // Casting from long double to uint64_t is miscompiled and drops bits.
      (!std::is_same<Float, long double>::value ||
       !std::is_same<Int, uint64_t>::value) &&
#endif
      std::numeric_limits<Float>::digits <= std::numeric_limits<Int>::digits;
}

template <typename Float>
struct Decomposed {
  using MantissaType =
      absl::conditional_t<std::is_same<long double, Float>::value, uint128,
                          uint64_t>;
  static_assert(std::numeric_limits<Float>::digits <= sizeof(MantissaType) * 8,
                "");
  MantissaType mantissa;
  int exponent;
};

// Decompose the double into an integer mantissa and an exponent.
template <typename Float>
Decomposed<Float> Decompose(Float v) {
  int exp;
  Float m = std::frexp(v, &exp);
  m = std::ldexp(m, std::numeric_limits<Float>::digits);
  exp -= std::numeric_limits<Float>::digits;

  return {static_cast<typename Decomposed<Float>::MantissaType>(m), exp};
}

// Print 'digits' as decimal.
// In Fixed mode, we add a '.' at the end.
// In Precision mode, we add a '.' after the first digit.
template <FormatStyle mode, typename Int>
int PrintIntegralDigits(Int digits, Buffer *out) {
  int printed = 0;
  if (digits) {
    for (; digits; digits /= 10) out->push_front(digits % 10 + '0');
    printed = out->size();
    if (mode == FormatStyle::Precision) {
      out->push_front(*out->begin);
      out->begin[1] = '.';
    } else {
      out->push_back('.');
    }
  } else if (mode == FormatStyle::Fixed) {
    out->push_front('0');
    out->push_back('.');
    printed = 1;
  }
  return printed;
}

// Back out 'extra_digits' digits and round up if necessary.
bool RemoveExtraPrecision(int extra_digits, bool has_leftover_value,
                          Buffer *out, int *exp_out) {
  if (extra_digits <= 0) return false;

  // Back out the extra digits
  out->end -= extra_digits;

  bool needs_to_round_up = [&] {
    // We look at the digit just past the end.
    // There must be 'extra_digits' extra valid digits after end.
    if (*out->end > '5') return true;
    if (*out->end < '5') return false;
    if (has_leftover_value || std::any_of(out->end + 1, out->end + extra_digits,
                                          [](char c) { return c != '0'; }))
      return true;

    // Ends in ...50*, round to even.
    return out->last_digit() % 2 == 1;
  }();

  if (needs_to_round_up) {
    RoundUp<FormatStyle::Precision>(out, exp_out);
  }
  return true;
}

// Print the value into the buffer.
// This will not include the exponent, which will be returned in 'exp_out' for
// Precision mode.
template <typename Int, typename Float, FormatStyle mode>
bool FloatToBufferImpl(Int int_mantissa, int exp, int precision, Buffer *out,
                       int *exp_out) {
  assert((CanFitMantissa<Float, Int>()));

  const int int_bits = std::numeric_limits<Int>::digits;

  // In precision mode, we start printing one char to the right because it will
  // also include the '.'
  // In fixed mode we put the dot afterwards on the right.
  out->begin = out->end =
      out->data + 1 + kMaxFixedPrecision + (mode == FormatStyle::Precision);

  if (exp >= 0) {
    if (std::numeric_limits<Float>::digits + exp > int_bits) {
      // The value will overflow the Int
      return false;
    }
    int digits_printed = PrintIntegralDigits<mode>(int_mantissa << exp, out);
    int digits_to_zero_pad = precision;
    if (mode == FormatStyle::Precision) {
      *exp_out = digits_printed - 1;
      digits_to_zero_pad -= digits_printed - 1;
      if (RemoveExtraPrecision(-digits_to_zero_pad, false, out, exp_out)) {
        return true;
      }
    }
    for (; digits_to_zero_pad-- > 0;) out->push_back('0');
    return true;
  }

  exp = -exp;
  // We need at least 4 empty bits for the next decimal digit.
  // We will multiply by 10.
  if (exp > int_bits - 4) return false;

  const Int mask = (Int{1} << exp) - 1;

  // Print the integral part first.
  int digits_printed = PrintIntegralDigits<mode>(int_mantissa >> exp, out);
  int_mantissa &= mask;

  int fractional_count = precision;
  if (mode == FormatStyle::Precision) {
    if (digits_printed == 0) {
      // Find the first non-zero digit, when in Precision mode.
      *exp_out = 0;
      if (int_mantissa) {
        while (int_mantissa <= mask) {
          int_mantissa *= 10;
          --*exp_out;
        }
      }
      out->push_front(static_cast<char>(int_mantissa >> exp) + '0');
      out->push_back('.');
      int_mantissa &= mask;
    } else {
      // We already have a digit, and a '.'
      *exp_out = digits_printed - 1;
      fractional_count -= *exp_out;
      if (RemoveExtraPrecision(-fractional_count, int_mantissa != 0, out,
                               exp_out)) {
        // If we had enough digits, return right away.
        // The code below will try to round again otherwise.
        return true;
      }
    }
  }

  auto get_next_digit = [&] {
    int_mantissa *= 10;
    int digit = static_cast<int>(int_mantissa >> exp);
    int_mantissa &= mask;
    return digit;
  };

  // Print fractional_count more digits, if available.
  for (; fractional_count > 0; --fractional_count) {
    out->push_back(get_next_digit() + '0');
  }

  int next_digit = get_next_digit();
  if (next_digit > 5 ||
      (next_digit == 5 && (int_mantissa || out->last_digit() % 2 == 1))) {
    RoundUp<mode>(out, exp_out);
  }

  return true;
}

template <FormatStyle mode, typename Float>
bool FloatToBuffer(Decomposed<Float> decomposed, int precision, Buffer *out,
                   int *exp) {
  if (precision > kMaxFixedPrecision) return false;

  // Try with uint64_t.
  if (CanFitMantissa<Float, std::uint64_t>() &&
      FloatToBufferImpl<std::uint64_t, Float, mode>(
          static_cast<std::uint64_t>(decomposed.mantissa),
          static_cast<std::uint64_t>(decomposed.exponent), precision, out, exp))
    return true;

#if defined(ABSL_HAVE_INTRINSIC_INT128)
  // If that is not enough, try with __uint128_t.
  return CanFitMantissa<Float, __uint128_t>() &&
         FloatToBufferImpl<__uint128_t, Float, mode>(
             static_cast<__uint128_t>(decomposed.mantissa),
             static_cast<__uint128_t>(decomposed.exponent), precision, out,
             exp);
#endif
  return false;
}

void WriteBufferToSink(char sign_char, absl::string_view str,
                       const FormatConversionSpecImpl &conv,
                       FormatSinkImpl *sink) {
  int left_spaces = 0, zeros = 0, right_spaces = 0;
  int missing_chars =
      conv.width() >= 0 ? std::max(conv.width() - static_cast<int>(str.size()) -
                                       static_cast<int>(sign_char != 0),
                                   0)
                        : 0;
  if (conv.has_left_flag()) {
    right_spaces = missing_chars;
  } else if (conv.has_zero_flag()) {
    zeros = missing_chars;
  } else {
    left_spaces = missing_chars;
  }

  sink->Append(left_spaces, ' ');
  if (sign_char != '\0') sink->Append(1, sign_char);
  sink->Append(zeros, '0');
  sink->Append(str);
  sink->Append(right_spaces, ' ');
}

template <typename Float>
bool FloatToSink(const Float v, const FormatConversionSpecImpl &conv,
                 FormatSinkImpl *sink) {
  // Print the sign or the sign column.
  Float abs_v = v;
  char sign_char = 0;
  if (std::signbit(abs_v)) {
    sign_char = '-';
    abs_v = -abs_v;
  } else if (conv.has_show_pos_flag()) {
    sign_char = '+';
  } else if (conv.has_sign_col_flag()) {
    sign_char = ' ';
  }

  // Print nan/inf.
  if (ConvertNonNumericFloats(sign_char, abs_v, conv, sink)) {
    return true;
  }

  int precision = conv.precision() < 0 ? 6 : conv.precision();

  int exp = 0;

  auto decomposed = Decompose(abs_v);

  Buffer buffer;

  FormatConversionChar c = conv.conversion_char();

  if (c == FormatConversionCharInternal::f ||
      c == FormatConversionCharInternal::F) {
    FormatF(decomposed.mantissa, decomposed.exponent,
            {sign_char, precision, conv, sink});
    return true;
  } else if (c == FormatConversionCharInternal::e ||
             c == FormatConversionCharInternal::E) {
    if (!FloatToBuffer<FormatStyle::Precision>(decomposed, precision, &buffer,
                                               &exp)) {
      return FallbackToSnprintf(v, conv, sink);
    }
    if (!conv.has_alt_flag() && buffer.back() == '.') buffer.pop_back();
    PrintExponent(
        exp, FormatConversionCharIsUpper(conv.conversion_char()) ? 'E' : 'e',
        &buffer);
  } else if (c == FormatConversionCharInternal::g ||
             c == FormatConversionCharInternal::G) {
    precision = std::max(0, precision - 1);
    if (!FloatToBuffer<FormatStyle::Precision>(decomposed, precision, &buffer,
                                               &exp)) {
      return FallbackToSnprintf(v, conv, sink);
    }
    if (precision + 1 > exp && exp >= -4) {
      if (exp < 0) {
        // Have 1.23456, needs 0.00123456
        // Move the first digit
        buffer.begin[1] = *buffer.begin;
        // Add some zeros
        for (; exp < -1; ++exp) *buffer.begin-- = '0';
        *buffer.begin-- = '.';
        *buffer.begin = '0';
      } else if (exp > 0) {
        // Have 1.23456, needs 1234.56
        // Move the '.' exp positions to the right.
        std::rotate(buffer.begin + 1, buffer.begin + 2, buffer.begin + exp + 2);
      }
      exp = 0;
    }
    if (!conv.has_alt_flag()) {
      while (buffer.back() == '0') buffer.pop_back();
      if (buffer.back() == '.') buffer.pop_back();
    }
    if (exp) {
      PrintExponent(
          exp, FormatConversionCharIsUpper(conv.conversion_char()) ? 'E' : 'e',
          &buffer);
    }
  } else if (c == FormatConversionCharInternal::a ||
             c == FormatConversionCharInternal::A) {
    return FallbackToSnprintf(v, conv, sink);
  } else {
    return false;
  }

  WriteBufferToSink(sign_char,
                    absl::string_view(buffer.begin, buffer.end - buffer.begin),
                    conv, sink);

  return true;
}

}  // namespace

bool ConvertFloatImpl(long double v, const FormatConversionSpecImpl &conv,
                      FormatSinkImpl *sink) {
  if (std::numeric_limits<long double>::digits ==
      2 * std::numeric_limits<double>::digits) {
    // This is the `double-double` representation of `long double`.
    // We do not handle it natively. Fallback to snprintf.
    return FallbackToSnprintf(v, conv, sink);
  }

  return FloatToSink(v, conv, sink);
}

bool ConvertFloatImpl(float v, const FormatConversionSpecImpl &conv,
                      FormatSinkImpl *sink) {
  return FloatToSink(v, conv, sink);
}

bool ConvertFloatImpl(double v, const FormatConversionSpecImpl &conv,
                      FormatSinkImpl *sink) {
  return FloatToSink(v, conv, sink);
}

}  // namespace str_format_internal
ABSL_NAMESPACE_END
}  // namespace absl
