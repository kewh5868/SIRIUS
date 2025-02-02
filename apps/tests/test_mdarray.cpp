#include <sirius.hpp>

using namespace sirius;

sddk::mdarray<int, 1> f1()
{
    sddk::mdarray<int, 1> aa;
    aa = sddk::mdarray<int, 1>(4);
    for (int i = 0; i < 4; i++) aa(i) = 200 + i;
    return aa;
}

void f2()
{
    sddk::mdarray<int, 1> a1(4);
    for (int i = 0; i < 4; i++) a1(i) = 100 + i;

    sddk::mdarray<int, 1> a2 = f1();
    for (int i = 0; i < 4; i++)
    {
        std::cout << "a1(" << i << ")=" << a1(i) << std::endl;
        std::cout << "a2(" << i << ")=" << a2(i) << std::endl;
    }
    sddk::mdarray<int, 1> a3(std::move(a2));
//== 
//== //    a1.deallocate();
//== //
//== //    std::cout << "Deallocate a1" << std::endl;
//== //
//== //    for (int i = 0; i < 4; i++)
//== //    {
//== //        std::cout << "a2(" << i << ")=" << a2(i) << std::endl;
//== //    }
//== //
//== //
//== //    sddk::mdarray<int, 1> a3 = a2;
//== //    
    for (int i = 0; i < 4; i++)
    {
        std::cout << "a3(" << i << ")=" << a3(i) << std::endl;
    }
 
    sddk::mdarray<int, 1> a4;
    a4 = std::move(a3);

    a4 = sddk::mdarray<int, 1>(20);
}

void f3()
{   
    for (int i = 0; i < 100; i++) {
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            sddk::mdarray<std::complex<double>, 2> a(100, 100);
            a(0, 0) = std::complex<double>(tid, tid);
        }
    }
}

void f4()
{
    sddk::mdarray<int, 1> buf;

    buf = sddk::mdarray<int, 1>(100, sddk::memory_t::host, "buf");

    buf = sddk::mdarray<int, 1>(200, sddk::memory_t::host, "buf");
    
    //buf = sddk::mdarray<int, 1>(300, memory_t::host | memory_t::device, "buf");


}

void f5()
{
    sddk::mdarray<double, 3> a;

    if (a.size(0) != 0 || a.size(1) != 0 || a.size(2) != 0) {
        printf("wrong sizes\n");
    }
}

template <typename T, int N>
void f6(sddk::mdarray<T, N>& a)
{
    std::array<sddk::mdarray_index_descriptor, N> dims;
    for (int i = 0; i < N; i++) {
        dims[i] = sddk::mdarray_index_descriptor(0, 10);
    }
    a = sddk::mdarray<T, N>(dims);
    a[0] = 100;
    a[a.size() - 1] = 200;
}

int main(int argn, char **argv)
{
    sirius::initialize(1);

    f2();

    f3();

    f4();

    f5();
    
    sddk::mdarray<double, 2> a;
    f6(a);

    sirius::finalize();
}
