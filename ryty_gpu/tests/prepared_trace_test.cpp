#include "../prepared_trace.h"
#include <cassert>
int main() {
    using namespace PreparedTrace;
    Aggregate a;
    a.Publish(Outcome{.enabled=false,.result=Accepted,.nonflat=9});
    auto empty=a.Drain(); for (auto n:empty) assert(n==0);
    a.Publish(Outcome{.enabled=true,.result=Accepted,.vertex_fallback=true,.nonflat=12,.flat_hit=8,.flat_miss=4});
    a.Publish(Outcome{.enabled=true,.result=EarlyUnmap});
    a.Publish(Outcome{.enabled=true,.result=LateBuffer,.nonflat=7});
    a.Publish(Outcome{.enabled=true,.result=VertexUnmap,.flat_miss=6});
    a.Publish(Outcome{.enabled=true,.result=RegionRebuild,.nonflat=3,.flat_miss=2});
    auto s=a.Drain();
    assert(s[Accepted]==1 && s[EarlyUnmap]==1 && s[LateBuffer]==1 && s[VertexUnmap]==1 && s[RegionRebuild]==1);
    assert(s[VertexFallback]==1 && s[NonflatBytes]==22 && s[FlatHitBytes]==8 && s[FlatMissBytes]==12 && s[NonacceptedBytes]==18);
    auto cleared=a.Drain(); for (auto n:cleared) assert(n==0);
}
