#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

/* Basic definitions of fixed point. */
typedef int fixed_t;
/* 16 LSB used for fractional part. */
#define FP_SHIFT_AMOUNT 14
/*x and y are fixed-point numbers, n is an integer.*/
/* Convert n to fixed-point. */
#define FP_CONVERT(n) ((fixed_t)((n) << FP_SHIFT_AMOUNT))
/* Convert x to integer (rounding toward zero). */
#define FP_INT_ZERO(x) ((x) >> FP_SHIFT_AMOUNT)
/* Convert x to integer (rounding to nearest). */
#define FP_INT_ROUND(x) ((x) >= 0 ? (((x) + (1 << (FP_SHIFT_AMOUNT - 1))) >> FP_SHIFT_AMOUNT) \
                                  : (((x) - (1 << (FP_SHIFT_AMOUNT - 1))) >> FP_SHIFT_AMOUNT))
/* Add x and y. */
#define FP_ADD(x, y) ((x) + (y))
/* Add x and n. */
#define FP_ADD_MIX(x, n) ((x) + (fixed_t)((n) << FP_SHIFT_AMOUNT))
/* Subtract y from x. */
#define FP_SUB(x, y) ((x) - (y))
/* Subtract n from x. */
#define FP_SUB_MIX(x, n) ((x) - (fixed_t)((n) << FP_SHIFT_AMOUNT))
/* Multiply x by y. */
#define FP_MUL(x, y) ((fixed_t)(((int64_t)(x)) * ((y) >> FP_SHIFT_AMOUNT)))
/* Multiply x by n. */
#define FP_MUL_MIX(x, n) ((x) * (n))
/* Divide x by y. */
#define FP_DIV(x, y) ((fixed_t)((((int64_t)(x)) << FP_SHIFT_AMOUNT) / (y)))
/* Divide x by n. */
#define FP_DIV_MIX(x, n) ((x) / (n))

#endif /**< threads/fixed_point.h */
