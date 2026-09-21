#include <cmath>
#include <complex>
#include <iostream>
#include <vector>
#include <iomanip>

static std::vector<double> rendered(const std::vector<double>& gains, const std::vector<int>& freqs, double fs){
    const double Q=4.318, pi=3.14159265358979323846;
    size_t n=gains.size(); std::vector<double> out(n,0.0), cs(n), alpha(n), amp(n); std::vector<std::complex<double>> z1(n), z2(n);
    for(size_t i=0;i<n;++i){ double w0=2*pi*freqs[i]/fs; cs[i]=cos(w0); alpha[i]=sin(w0)/(2*Q); amp[i]=pow(10.0,gains[i]/40.0); z1[i]=std::polar(1.0,-w0); z2[i]=std::polar(1.0,-2*w0); }
    for(size_t b=0;b<n;++b){ double sum=0; for(size_t i=0;i<n;++i){ if(fabs(gains[i])<1e-9) continue; double m1=-2*cs[i]; auto num=(1.0+alpha[i]*amp[i])+m1*z1[b]+(1.0-alpha[i]*amp[i])*z2[b]; auto den=(1.0+alpha[i]/amp[i])+m1*z1[b]+(1.0-alpha[i]/amp[i])*z2[b]; sum += 20.0*log10(abs(num/den)); } out[b]=sum; }
    return out;
}
static double energy(const std::vector<double>& resp, const std::vector<double>& measured, const std::vector<bool>& usable){ double p0=0,p1=0; for(size_t i=0;i<resp.size();++i){ if(!usable[i]||measured[i]<=-190) continue; double w=pow(10.0, measured[i]/10.0); p0+=w; p1+=w*pow(10.0, resp[i]/10.0);} return (p0>1e-30&&p1>1e-30)?10*log10(p1/p0):0; }
int main(){
    std::vector<int> f={20,25,31,40,50,63,80,100,125,160,200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500,16000,20000};
    std::vector<double> g(31,0.0); g[30]=6.0; // worst isolated 20k boost
    std::vector<double> meas(31,-60.0); // equal power for visibility
    std::vector<bool> u44(31,true), u48(31,true); for(size_t i=0;i<f.size();++i){ u44[i]=f[i]<=44100*0.45; u48[i]=f[i]<=48000*0.45; }
    auto r44=rendered(g,f,44100), r48=rendered(g,f,48000);
    std::cout<<std::fixed<<std::setprecision(3);
    std::cout<<"20k-only +6dB, rendered@20k fs44="<<r44[30]<<" fs48="<<r48[30]<<"\n";
    std::cout<<"budgeted energy if analyzer mask 44.1="<<energy(r44,meas,u44)<<" dB, 48="<<energy(r48,meas,u48)<<" dB\n";
    std::cout<<"usable44 last="; for(size_t i=0;i<f.size();++i) if(u44[i]) std::cout<<f[i]<<" "; std::cout<<"\n";
    std::cout<<"usable48 last="; for(size_t i=0;i<f.size();++i) if(u48[i]) std::cout<<f[i]<<" "; std::cout<<"\n";
}
