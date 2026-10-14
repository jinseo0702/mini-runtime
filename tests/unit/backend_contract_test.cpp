#include "mini_runtime/backend.hpp"
#include <cassert>
#include <cstdlib>
#include <new>
#include <type_traits>
static int budget = -1;
void* operator new(std::size_t n) {
    if (budget == 0) throw std::bad_alloc{};
    if (budget > 0) --budget;
    if (auto p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
int main() {
    using namespace mini_runtime;
    using R = Result<void, RuntimeError>;
    static_assert(std::has_virtual_destructor_v<IExecutionBackend>);
    static_assert(std::has_virtual_destructor_v<IBackendFactory>);
    static_assert(std::is_same_v<decltype(&IExecutionBackend::initialize), R (IExecutionBackend::*)()>);
    static_assert(std::is_same_v<decltype(&IExecutionBackend::operations),
        const std::vector<OperationSchema>& (IExecutionBackend::*)() const noexcept>);
    static_assert(std::is_same_v<decltype(&IExecutionBackend::submit),
        SubmitDecision (IExecutionBackend::*)(const BackendCommand&, CompletionCallback) noexcept>);
    static_assert(std::is_same_v<decltype(&IExecutionBackend::quiesce), void (IExecutionBackend::*)() noexcept>);
    static_assert(std::is_same_v<decltype(&IExecutionBackend::cleanup), void (IExecutionBackend::*)() noexcept>);
    static_assert(std::is_same_v<decltype(&IBackendFactory::create),
        Result<std::unique_ptr<IExecutionBackend>, RuntimeError> (IBackendFactory::*)()>);
    static_assert(std::is_same_v<decltype(BackendCommand::command), std::shared_ptr<const CommandDescriptor>>);
    static_assert(std::is_same_v<ParameterValue, std::variant<int64_t,double,bool,std::string>>);
    constexpr ErrorCode codes[] = {ErrorCode::INVALID_STATE, ErrorCode::START_IN_PROGRESS,
        ErrorCode::ALREADY_RUNNING, ErrorCode::SHUTDOWN_IN_PROGRESS, ErrorCode::ALREADY_STOPPED,
        ErrorCode::INVALID_EVENT, ErrorCode::INVALID_PARAMETER, ErrorCode::UNKNOWN_OPERATION,
        ErrorCode::OUT_OF_MEMORY, ErrorCode::BACKEND_INIT_FAILED, ErrorCode::THREAD_INIT_FAILED,
        ErrorCode::BACKEND_REJECTED, ErrorCode::BACKEND_FAILURE, ErrorCode::SHUTDOWN_CANCELLED,
        ErrorCode::SCHEMA_CONFLICT, ErrorCode::RUNTIME_ID_EXHAUSTED, ErrorCode::EVENT_ID_EXHAUSTED,
        ErrorCode::JOB_ID_EXHAUSTED, ErrorCode::SUBMISSION_ID_EXHAUSTED};
    for (unsigned i=0; i<19; ++i) assert(static_cast<unsigned>(codes[i])==i);
    budget=0;
    auto fallback=make_error(ErrorCode::SCHEMA_CONFLICT, "long diagnostic that requires allocation", 9);
    budget=-1;
    assert(fallback.code==ErrorCode::SCHEMA_CONFLICT && fallback.message.empty() && fallback.job_id==9);
    OperationSchema noop{{"test.noop",1},{},false,false};
    OperationSchema other{{"other",1},{{"a",ParameterType::Integer,true},{"b",ParameterType::String,false}},false,false};
    detail::OperationRegistry registry;
    assert(registry.register_operations({noop,other,noop}));
    assert(registry.size()==2 && registry.contains(noop.key));
    assert(!(noop.key==OperationKey{"test.noop",2}));
    assert(!(noop.key==OperationKey{"different",1}));
    for(int field=0; field<6; ++field) {
        auto conflict=other;
        if(field==0) conflict.parameters[0].name="c";
        if(field==1) conflict.parameters[0].type=ParameterType::Number;
        if(field==2) conflict.parameters[0].required=false;
        if(field==3) std::swap(conflict.parameters[0],conflict.parameters[1]);
        if(field==4) conflict.allows_buffers=true;
        if(field==5) conflict.allows_dependencies=true;
        OperationSchema fresh{{"fresh",1},{},false,false};
        auto r=registry.register_operations({fresh,conflict});
        assert(!r && r.error().code==ErrorCode::SCHEMA_CONFLICT && registry.size()==2);
        assert(!registry.contains(fresh.key));
        detail::OperationRegistry empty;
        auto within=empty.register_operations({other,conflict});
        assert(!within && within.error().code==ErrorCode::SCHEMA_CONFLICT && empty.size()==0);
    }
    std::vector<OperationSchema> batch{other,{{"new operation with a long name",1},{},false,false}};
    bool succeeded=false;
    for(int limit=0; limit<50 && !succeeded; ++limit) {
        budget=limit;
        auto r=registry.register_operations(batch);
        budget=-1;
        succeeded=bool(r);
        if(!r) assert(r.error().code==ErrorCode::OUT_OF_MEMORY && registry.size()==2);
    }
    assert(succeeded && registry.size()==3);
    CommandDescriptor command{noop.key,{},{},{},{}};
    assert(registry.validate(command));
    for(int field=0;field<7;++field) {
        auto invalid=command;
        if(field==0) invalid.operation.name="missing";
        if(field==1) invalid.operation.version=2;
        if(field==2) invalid.parameters.push_back({"x",int64_t{1}});
        if(field==3) invalid.inputs.push_back(1);
        if(field==4) invalid.outputs.push_back(1);
        if(field==5) invalid.dependencies.push_back(1);
        if(field==6) invalid.operation=other.key;
        auto r=registry.validate(invalid);
        assert(!r && r.error().code==(field<2?ErrorCode::UNKNOWN_OPERATION:ErrorCode::INVALID_PARAMETER));
    }
    auto owned=std::make_shared<const CommandDescriptor>(command);
    BackendCommand backend{1,2,owned};
    command.operation.name="mutated";
    assert(backend.command->operation==noop.key);
    registry.clear();
    assert(registry.size()==0 && !registry.contains(noop.key));
}
