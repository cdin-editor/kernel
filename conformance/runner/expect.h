#ifndef EXPECT_H
#define EXPECT_H

/* Checks one `expect <status> key=value ...` line against the shared output
 * block. Unknown keys are syntax errors. */
void expect_check(const char *line, const char *path, int line_num, int *errors);

#endif
