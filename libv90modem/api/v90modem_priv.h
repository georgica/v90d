/* libv90modem: what the library's own tools may see behind the public
 * handles (the replay tool, the benches, the test suite). Never installed;
 * not part of the API. */
#ifndef V90MODEM_PRIV_H
#define V90MODEM_PRIV_H
#include "../include/v90modem.h"
#include "../modem/answer.h"
#include "../v42/v42.h"

answer_t *v90m__answer(v90m_t *m);
v42ec_t  *v90m__ec(v90m_ec_t *e);

#endif
