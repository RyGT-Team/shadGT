#define main descriptor_action_existing_fixture_main
#include "parallel_descriptor_actions_test.cpp"
#undef main
#include <cstddef>
#include "descriptor-action-under-test.inc"

static_assert(sizeof(NativeDescriptorAction)==16);
static_assert(sizeof(NativeDescriptorAction)==sizeof(PreparedDraw::DescriptorAction));
#define CHECK_OFFSET(field) static_assert(offsetof(NativeDescriptorAction,field)==offsetof(PreparedDraw::DescriptorAction,field))
CHECK_OFFSET(upload_size); CHECK_OFFSET(buffer_first); CHECK_OFFSET(stage_local);
CHECK_OFFSET(range_hint); CHECK_OFFSET(region_hint); CHECK_OFFSET(stage_index);
CHECK_OFFSET(upload_match); CHECK_OFFSET(is_buffer);
#undef CHECK_OFFSET

int main() {
    PreparedDraw p;
    Shader::Info stage;
    stage.identity=7;
    stage.buffers.resize(65535);
    const std::array<const Shader::Info*,5> stages{&stage,nullptr,nullptr,nullptr,nullptr};
    p.bindings.buffers.resize(65535);
    for (std::size_t index : {std::size_t{0}, std::size_t{1}, std::size_t{65534}}) {
        p.bindings.writes={row(&p.bindings.buffers[index])};
        produce(&p,stages);
        assert(p.cpu_proof.descriptor_structure && p.descriptor_actions.size()==1);
        const auto& action=p.descriptor_actions[0];
        // Original mapping scans stage ranges with size_t arithmetic.
        std::size_t base=0, expected_stage=255, expected_local=0;
        for(std::size_t i=0;i<stages.size();++i) {
            const std::size_t count=stages[i] ? stages[i]->buffers.size() : 0;
            if(index>=base && index-base<count) {expected_stage=i;expected_local=index-base;break;}
            base+=count;
        }
        assert(action.buffer_first==index && action.stage_index==expected_stage && action.stage_local==expected_local);
    }
    p.bindings.writes={row(nullptr)};
    produce(&p,stages);
    assert(!p.cpu_proof.descriptor_structure && p.descriptor_actions.empty());
    p.bindings.writes={row(&p.bindings.buffers[65534],2)};
    produce(&p,stages);
    assert(!p.cpu_proof.descriptor_structure && p.descriptor_actions.empty());
    p.bindings.buffers.resize(65536);
    p.bindings.buffers[65535].range=19;
    p.uploads={{65535,19}};
    p.bindings.writes={row(&p.bindings.buffers[65535])};
    produce(&p,stages);
    assert(p.cpu_proof.descriptor_structure && p.descriptor_actions.empty());
    assert(oracle(p,65535,19));
    assert(query(&p,nullptr,65535,&p.bindings.buffers[65535])==oracle(p,65535,19));
    p.bindings.buffers[65535].range=20;
    assert(!oracle(p,65535,20));
    assert(query(&p,nullptr,65535,&p.bindings.buffers[65535])==oracle(p,65535,20));
    puts("PASS source layout and producer 65535/65536 boundaries");
}
