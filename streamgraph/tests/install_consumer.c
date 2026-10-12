#include <turbo_streamgraph.h>
#include <assert.h>
int main(void) {
    tm_sg_port_v1 p = TM_SG_PORT_V1_INIT;
    tm_sg_diagnostic_v1 d = TM_SG_DIAGNOSTIC_V1_INIT;
    tm_sg_program *program = NULL;
    assert(p.size == sizeof(p));
    assert(d.version == TM_SG_ABI_VERSION);
    assert(tm_sg_compile(NULL, 0, NULL, 0, NULL, &program, &d) == TM_SG_ELIMIT);
    return program != NULL;
}
