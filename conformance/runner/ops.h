#ifndef OPS_H
#define OPS_H

/* Executes one operation line. Returns 0 when the line was a known op, -1 on a
 * syntax error (already reported). */
int ops_execute(const char *line, const char *path, int line_num, int *errors);

#endif
