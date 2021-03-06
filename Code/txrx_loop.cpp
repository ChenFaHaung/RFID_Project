#include <uhd/utils/thread_priority.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/types/time_spec.hpp>

#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>

#include <iostream>
#include <complex>
#include <cmath>
#include <algorithm>

#include <csignal>
#include <stdio.h>
#include <stdlib.h>
#include "txrx_loop.h"
#include "time.h"

namespace po = boost::program_options;

//System parameters
double freq, gain, thres;
double inter;
double rate;
double du, du1;
time_t start, fin;
int s_rate = adc_rate;
enum GEN2_LOGIC_STATUS
{
	SEND_QUERY,
	SEND_ACK,
	INIT,
	IDLE,
	SEND_CW,
	FAIL,
	DECODE_RN16
};
GEN2_LOGIC_STATUS gen2_logic_status = INIT;

//USRP cmd
uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
uhd::time_spec_t time_start_recv;

//USRP
uhd::usrp::multi_usrp::sptr usrp_tx;
uhd::usrp::multi_usrp::sptr usrp_rx;
string usrp_tx_ip;
string usrp_rx_ip;

//TX/RX metadata
uhd::rx_metadata_t rx_md;
uhd::tx_metadata_t tx_md;

//Buffer
gr_complex pkt_tx[MAX_PKT_LEN];
gr_complex *pkt_rx;
gr_complex *query_rx;
gr_complex *pkt_rxtmp;
gr_complex *pkt_rxsack;
gr_complex *decoding_rx;
gr_complex zeros[SYM_LEN];
gr_complex ones[SYM_LEN];

//File
FILE *in_file;  // tx freq signals
FILE *out_file; // rx time signal after preambles
string in_name, out_name, raw_name;

//Evaluation
size_t r_cnt;
double r_sec;
size_t s_cnt;
size_t rn16EndSize;
size_t query_cnt = 0;

// correlation parameter
static bool stop_signal = false;

/*************************************************************************
 * Query Part Setting
 * parameter
 * crc_append
 * gen_query_bits
 * gen_query_adjust_bits
 ************************************************************************/

//generating vector
vector<int> RN16_bits, EPC_bits;
std::vector<float> query_bits, query_adjust_bits, cw_ack, cucw_ack, cw_query, preamble, delim, data_0,
	data_1, rtcal, trcal, ack_bits, frame_sync, p_down, cw, query_rep, nak;
vector<complex<float> > Query, SACK, beforeGate, afterGate, raw_rx;

void crc_append(std::vector<float> &q)
{
	int crc[] = {1, 0, 0, 1, 0};
	for (int i = 0; i < 17; i++)
	{
		int tmp[] = {0, 0, 0, 0, 0};
		tmp[4] = crc[3];
		if (crc[4] == 1)
		{
			if (q[i] == 1)
			{
				tmp[0] = 0;
				tmp[1] = crc[0];
				tmp[2] = crc[1];
				tmp[3] = crc[2];
			}
			else
			{
				tmp[0] = 1;
				tmp[1] = crc[0];
				tmp[2] = crc[1];
				if (crc[2] == 1)
					tmp[3] = 0;
				else
					tmp[3] = 1;
			}
		}
		else
		{
			if (q[i] == 1)
			{
				tmp[0] = 1;
				tmp[1] = crc[0];
				tmp[2] = crc[1];
				if (crc[2] == 1)
					tmp[3] = 0;
				else
					tmp[3] = 1;
			}
			else
			{
				tmp[0] = 0;
				tmp[1] = crc[0];
				tmp[2] = crc[1];
				tmp[3] = crc[2];
			}
		}
		memcpy(crc, tmp, 5 * sizeof(float));
	}
	for (int i = 4; i >= 0; i--)
		q.push_back(crc[i]);
	return;
}

void gen_query_bits(void)
{
	query_bits.resize(0);
	query_bits.insert(query_bits.end(), &QUERY_CODE[0], &QUERY_CODE[4]);
	query_bits.push_back(DR);
	query_bits.insert(query_bits.end(), &M[0], &M[2]);
	query_bits.push_back(TREXT);
	query_bits.insert(query_bits.end(), &SEL[0], &SEL[2]);
	query_bits.insert(query_bits.end(), &SESSION[0], &SESSION[2]);
	query_bits.push_back(TARGET);
	query_bits.insert(query_bits.end(), &Q_VALUE[FIXED_Q][0], &Q_VALUE[FIXED_Q][4]);
	crc_append(query_bits);
	return;
}

void gen_ack_bits(const int *in)
{
	ack_bits.resize(0);
	ack_bits.insert(ack_bits.end(), frame_sync.begin(), frame_sync.end());
	ack_bits.insert(ack_bits.end(), &ACK_CODE[0], &ACK_CODE[2]);
	ack_bits.insert(ack_bits.end(), &in[0], &in[16]);

	return;
}

void gen_query_adjust_bits(void)
{
	query_adjust_bits.resize(0);
	query_adjust_bits.insert(query_adjust_bits.end(), &QADJ_CODE[0], &QADJ_CODE[4]);
	query_adjust_bits.insert(query_adjust_bits.end(), &SESSION[0], &SESSION[2]);
	query_adjust_bits.insert(query_adjust_bits.end(), &Q_UPDN[1][0], &Q_UPDN[1][3]);
	return;
}

/*************************************************************************
 * Assemble msg
 * gen_query_bits
 * gen_query_adjust_bits
 ************************************************************************/

void readerInit(void)
{
	double sample_d = 1.0 / dac_rate * pow(10, 6);

	// Number of samples for transmitting
	float n_data0_s = 2 * PW_D / sample_d;
	float n_data1_s = 4 * PW_D / sample_d;
	float n_delim_s = DELIM_D / sample_d;
	float n_trcal_s = TRCAL_D / sample_d;
	float n_pw_s = PW_D / sample_d;
	float n_cw_s = CW_D / sample_d;

	// CW waveforms of different sizes
	const int n_cwquery_s = (T1_D + T2_D + RN16_D) / sample_d;  //RN16
	const int n_cwack_s = (3 * T1_D + T2_D + EPC_D) / sample_d; //EPC

	//if it is longer than nominal it wont cause tags to change inventoried flag
	const int n_p_down_s = (P_DOWN_D) / sample_d;
	p_down.resize(n_p_down_s); // Power down samples

	// Construct vectors (resize() default initialization is zero)
	data_0.resize(n_data0_s);
	data_1.resize(n_data1_s);
	cw.resize(n_cw_s);
	cw_query.resize(n_cwquery_s); // Sent after query/query rep
	cw_ack.resize(n_cwack_s);	 // Sent after ack
	delim.resize(n_delim_s);
	rtcal.resize(n_data0_s + n_data1_s);
	trcal.resize(n_trcal_s);

	// Fill vectors with data
	std::fill_n(data_0.begin(), data_0.size() / 2, 1);
	std::fill_n(data_1.begin(), 3 * data_1.size() / 4, 1);
	std::fill_n(cw.begin(), cw.size(), 1);
	std::fill_n(cw_query.begin(), cw_query.size(), 1);
	std::fill_n(cw_ack.begin(), cw_ack.size(), 1);
	std::fill_n(rtcal.begin(), rtcal.size() - n_pw_s, 1); // RTcal
	std::fill_n(trcal.begin(), trcal.size() - n_pw_s, 1); // TRcal

	// create preamble
	preamble.insert(preamble.end(), delim.begin(), delim.end());
	preamble.insert(preamble.end(), data_0.begin(), data_0.end());
	preamble.insert(preamble.end(), rtcal.begin(), rtcal.end());
	preamble.insert(preamble.end(), trcal.begin(), trcal.end());

	// create framesync
	frame_sync.insert(frame_sync.end(), delim.begin(), delim.end());
	frame_sync.insert(frame_sync.end(), data_0.begin(), data_0.end());
	frame_sync.insert(frame_sync.end(), rtcal.begin(), rtcal.end());

	// create query rep
	query_rep.insert(query_rep.end(), frame_sync.begin(), frame_sync.end());
	query_rep.insert(query_rep.end(), data_0.begin(), data_0.end());
	query_rep.insert(query_rep.end(), data_0.begin(), data_0.end());
	query_rep.insert(query_rep.end(), data_0.begin(), data_0.end());
	query_rep.insert(query_rep.end(), data_0.begin(), data_0.end());

	// create nak
	nak.insert(nak.end(), frame_sync.begin(), frame_sync.end());
	nak.insert(nak.end(), data_1.begin(), data_1.end());
	nak.insert(nak.end(), data_1.begin(), data_1.end());
	nak.insert(nak.end(), data_0.begin(), data_0.end());
	nak.insert(nak.end(), data_0.begin(), data_0.end());
	nak.insert(nak.end(), data_0.begin(), data_0.end());
	nak.insert(nak.end(), data_0.begin(), data_0.end());
	nak.insert(nak.end(), data_0.begin(), data_0.end());
	nak.insert(nak.end(), data_0.begin(), data_0.end());

	gen_query_bits();
	gen_query_adjust_bits();

	// kate 1115
	rn16EndSize = cw_ack.size() + preamble.size() + query_bits.size() + (RN16_BITS + TAG_PREAMBLE_BITS + 10) * TAG_BIT_D;
	rn16EndSize *= (s_rate / pow(10, 6));
	return;
}

void gen_query(void)
{
	Query.clear();
	for (size_t i = 0; i < cw_ack.size(); i++)
	{
		Query.push_back(cw_ack[i]);
	}
	for (size_t i = 0; i < preamble.size(); i++)
	{
		Query.push_back(preamble[i]);
	}
	for (size_t i = 0; i < query_bits.size(); i++)
	{
		if (query_bits[i] == 1)
			for (size_t j = 0; j < data_1.size(); j++)
				Query.push_back(data_1[j]);
		else
			for (size_t j = 0; j < data_0.size(); j++)
				Query.push_back(data_0[j]);
	}
	for (size_t i = 0; i < cw_query.size(); i++)
		Query.push_back(cw_query[i]);

}

void gen_ACK(void)
{
	SACK.clear();

	//for(int i=0; i < 200; i++)
	//	SACK.push_back(0);
	//Send FrameSync
	SACK.insert(SACK.end(), frame_sync.begin(), frame_sync.end());
	//ack
	SACK.insert(SACK.end(), data_0.begin(), data_0.end());
	SACK.insert(SACK.end(), data_1.begin(), data_1.end());
}

//Willy: down sampling
void filter(void)
{
	// Willy : s_cnt -> size of pkt_rx
	int now = 0, top, down, size = query_cnt / decim; // TODO: raw_rx.size()
	beforeGate.clear();
	for (int i = 0; i < size; i++)
	{
		complex<float> tmp(0, 0);
		if (now < 25)
		{ //25
			top = 24;
			down = -1;
		}
		else
		{
			top = now;
			down = now - 25;
		}
		for (int j = top; j > down; j--)
			tmp += query_rx[j];
		beforeGate.push_back(tmp);
		now += decim;
	}
	return;
}

//Willy : gate the sample
float gate_impl(void)
{
	vector<float> win_samples;
	int n_items = beforeGate.size();
	int n_samples_T1 = T1_D * (s_rate / pow(10, 6)) / decim; // 250/5 for filter
	int n_samples_PW = PW_D * (s_rate / pow(10, 6)) / decim; // 24
	int n_samples_TAG_BIT = TAG_BIT_D * (s_rate / pow(10, 6)) / decim;
	int win_length = WIN_SIZE_D * (s_rate / pow(10, 6)) / decim;
	win_samples.resize(win_length);
	int gate_status = 0;  //close
	int signal_state = 0; // NEG_EDGE

	//cout << "n_items " << n_items << endl;

	int n_samples_to_ungate = (RN16_BITS + TAG_PREAMBLE_BITS) * n_samples_TAG_BIT + 2 * n_samples_TAG_BIT;
	int n_samples = 0, win_index = 0, tagIndex;
	float num_pulses = 0, THRESH_FRACTION = 0.75, sample_thresh = 1.1, sample_ampl = 0, avg_ampl = 0, cwAmpl = 0; // origin: THRESH_FRACTION = 0.75
	//n_samples_to_ungate /= 5;//filter
	//n_samples_TAG_BIT /= 5;//filter
	afterGate.clear(); // avoid store RN16 multiple times

	/*
	cout << "n_items: " << n_items << endl;
	cout << "n_samples_T1: " << n_samples_T1 << endl;
	cout << "n_samples_PW: " << n_samples_PW << endl;
	cout << "n_samples_TAG_BIT: " << n_samples_TAG_BIT << endl;
	cout << "win_length: " << win_length << endl;
	cout << "n_samples_to_ungate: " << n_samples_to_ungate << endl;
	cout << "THRESH_FRACTION: " << THRESH_FRACTION << endl;
*/

	for (int i = 0; i < n_items; i++)
	{
		// Tracking average amplitude
		sample_ampl = abs(beforeGate[i]);
		avg_ampl = avg_ampl + (sample_ampl - win_samples[win_index]) / win_length;
		win_samples[win_index] = sample_ampl;
		win_index = (win_index + 1) % win_length;
		//Threshold for detecting negative/positive edges
		sample_thresh = avg_ampl * THRESH_FRACTION;

		//cout << i << ": n_samples: " << n_samples <<  ",  avg_ampl: " << avg_ampl << ", sample_ampl: " << sample_ampl <<  endl;

		if (gate_status != 1)
		{ // gate close
			//Tracking DC offset (only during T1)
			n_samples++;
			// Potitive edge -> Negative edge
			if (sample_ampl < sample_thresh && signal_state == 1)
			{
				n_samples = 0;
				signal_state = 0;
				//cout << "postive -> negative ======= " << i << endl;
			}
			// Negative edge -> Positive edge
			else if (sample_ampl > sample_thresh && signal_state == 0)
			{

				//cout << "negative -> positive ======= " << i << endl;

				signal_state = 1;
				if (n_samples > n_samples_PW / 2)
					num_pulses++;
				else
					num_pulses = 0;
				n_samples = 0;
			}
			if (n_samples > n_samples_T1 && signal_state == 1 && num_pulses > NUM_PULSES_COMMAND)
			{
				//cout << "GATE OPEN" << endl;
				tagIndex = i;
				gate_status = 1;
				afterGate.push_back(beforeGate[i]);
				num_pulses = 0;
				n_samples = 1; // Count number of samples passed to the next block
			}
		}
		else
		{ // gate open
			n_samples++;
			afterGate.push_back(beforeGate[i]);
			if (n_samples >= n_samples_to_ungate)
			{
				//cout << "cwAmpl: " << cwAmpl << ", n_samples_TAG_BIT: " << n_samples_TAG_BIT << ", tagIndex: " << tagIndex << endl;
				for (int i = tagIndex - n_samples_TAG_BIT; i < tagIndex; i++)
					cwAmpl += abs(beforeGate[i]);
				gate_status = 0;
				break;
			}
		}
	}
	// cout << "aftergate: " << afterGate.size() << endl;
	return cwAmpl;
}

int correlate(int n_samples_TAG_BIT, float cwAmpl)
{
	vector<int> preamble;
	float sum = 0.0;
	int type;
	for (int i = 0; i < 60; ++i)
		sum += abs(afterGate[i]);

	for (int i = 0; i < TAG_PREAMBLE_BITS * 2; i++)
	{
		for (int j = 0; j < 30; j++)
		{
			if (sum > cwAmpl)
				type = 1;
			else
				type = 0;
		}
	}

	//correlation
	float Max = -10000000;
	int size = afterGate.size(), index = 0;
	for (int i = 0; i < 20; i++)
	{
		if (i != 0)
		{
			sum = sum - abs(afterGate[i - 1]) + abs(afterGate[i + 59]);
		}
		float tmp = 0.0, aver = sum / 60;
		//cout << "sum: " << sum << ", avg: " << aver << endl;
		for (int j = 0; j < 60; j++)
		{
			tmp += float(PREAMBLE[type][j / 5]) * (abs(afterGate[i + j]) - aver);
		}
		//cout << "tmp: " << tmp << ", Max: " << Max << endl;
		if (tmp > Max)
		{
			//cout << " >> tmp: " << tmp << ", Max: " << Max << endl;
			Max = tmp;
			index = i;
		}
	}
	//cout << "RN16 index: " << index << endl;
	return index;
}

void rn16Decode(int rn16Index)
{
	vector<int> tmpBits;
	int windowSize = 10, now = rn16Index;
	RN16_bits.resize(0);
	//detect +1 or 0 in data
	for (int i = 0; i < RN16_BITS * 2; ++i)
	{
		float sum = 0.0, aver;
		for (int j = now - windowSize; j < now + windowSize; ++j)
			sum += abs(afterGate[j]);
		aver = sum / (windowSize * 2);
		int count = 0;
		for (int j = now; j < now + 5; ++j)
			if (abs(afterGate[j]) > aver)
				count++;
		if (count > 2)
			tmpBits.push_back(1);
		else
			tmpBits.push_back(0);
		now += 5;
	}
	//decode by fm0 encoding
	for (int i = 0; i < RN16_BITS * 2 - 2; i += 2)
	{
		if (tmpBits[i] != tmpBits[i + 1])
		{
			RN16_bits.push_back(0);
			cout << "0 ";
		}
		else
		{
			RN16_bits.push_back(1);
			cout << "1 ";
		}
	}
	cout << endl;
	// kate tmp
	// afterGate.resize(0);
	return;
}

void epcDecode(int epcIndex)
{
	vector<int> tmpBits;
	int windowSize = 10, now = epcIndex;
	EPC_bits.resize(0);
	//detect +1 or 0 in data
	for (int i = 0; i < EPC_BITS * 2 - 2; i++)
	{
		float sum = 0.0, aver;
		for (int j = now - windowSize; j < now + windowSize; j++)
		{
			sum += abs(afterGate[j]);
		}
		aver = sum / (windowSize * 2);
		int count = 0;
		for (int j = now; j < now + 5; j++)
			if (abs(afterGate[j]) > aver)
				count++;
		if (count > 2)
			tmpBits.push_back(1);
		else
			tmpBits.push_back(0);
		now += 5;
	}
	//decode by fm0 encoding
	for (int i = 0; i < EPC_BITS * 2 - 2; i += 2)
	{
		if (tmpBits[i] != tmpBits[i + 1])
			EPC_bits.push_back(0);
		else
			EPC_bits.push_back(1);
	}
	afterGate.resize(0);
	return;
}

void init_usrp()
{
	usrp_tx = uhd::usrp::multi_usrp::make(usrp_tx_ip);
	usrp_rx = uhd::usrp::multi_usrp::make(usrp_rx_ip);
	usrp_tx->set_rx_rate(rate * 2);
	usrp_rx->set_rx_rate(rate * 2);
	usrp_tx->set_tx_rate(rate);
	usrp_rx->set_tx_rate(rate);

	usrp_tx->set_rx_freq(freq);
	usrp_rx->set_rx_freq(freq);
	usrp_tx->set_tx_freq(freq);
	usrp_rx->set_tx_freq(freq);

	usrp_tx->set_rx_gain(gain);
	usrp_rx->set_rx_gain(gain);
	usrp_tx->set_tx_gain(gain);
	usrp_rx->set_tx_gain(gain);

	// turn off the DC offset tune
	usrp_rx->set_rx_dc_offset(false);
}

void sync_clock()
{
	cout << "SYNC Clock" << endl;

	usrp_rx->set_clock_config(uhd::clock_config_t::external());
	usrp_tx->set_clock_config(uhd::clock_config_t::external());

	usrp_rx->set_time_next_pps(uhd::time_spec_t(0.0));
	usrp_tx->set_time_next_pps(uhd::time_spec_t(0.0));
}

void init_stream()
{
	boost::this_thread::sleep(boost::posix_time::milliseconds(WARM_UP_TIME));
	stream_cmd.time_spec = time_start_recv = uhd::time_spec_t(1.0) + usrp_rx->get_time_now();
	cout << "Time to start receiving: " << time_start_recv.get_real_secs() << endl;
	stream_cmd.stream_now = false;
	usrp_rx->issue_stream_cmd(stream_cmd);
}

void init_sys()
{
	// Tx buffer initialize
	Query.clear();
	SACK.clear();
	memset(zeros, 0, sizeof(zeros));
	memset(ones, 0, sizeof(ones));
	for (size_t i = 0; i < SYM_LEN; i++)
	{
		ones[i].real(1);
	}

	// Rx buffer initialize
	printf("New receiving signal buffer\n");
	s_cnt = (size_t)(1e8 / SYM_LEN * r_sec);
	pkt_rx = new gr_complex[s_cnt];
	// kate 1115
	query_rx = new gr_complex[QUERY_RXSIZE];
	pkt_rxtmp = new gr_complex[SYM_LEN * 2];

	if (pkt_rx != NULL)
	{
		memset(pkt_rx, 0, sizeof(gr_complex) * s_cnt);
	}

	if (query_rx != NULL)
	{
		memset(query_rx, 0, sizeof(gr_complex) * QUERY_RXSIZE);
	}

	if (pkt_rxtmp != NULL)
	{
		memset(pkt_rxtmp, 0, sizeof(gr_complex) * SYM_LEN * 2);
	}

	// decoding Rx buffer
	decoding_rx = new gr_complex[RX_WND_SIZE];

	init_usrp();

	// use external clock for synchronziation
	sync_clock();
	init_stream();
}

void dump_signals()
{
	cout << endl
		 << "Dump signals" << endl;
	FILE *out_file;
	char tmp_n[1000];
	sprintf(tmp_n, "%s", out_name.c_str());
	out_file = fopen(tmp_n, "wb");
	fwrite(pkt_rx, sizeof(gr_complex), s_cnt, out_file);
	fclose(out_file);
}

void end_sys()
{
	printf("Delete receiving signal buffer\n");
	if (pkt_rx != NULL)
	{
		delete[] pkt_rx;
	}
	if (query_rx != NULL)
	{
		delete[] query_rx;
	}
	if (pkt_rxtmp != NULL)
	{
		delete[] pkt_rxtmp;
	}
	if (decoding_rx != NULL)
	{
		delete[] decoding_rx;
	}
}

void sig_int_handler(int) { stop_signal = true; }

int UHD_SAFE_MAIN(int argc, char *argv[])
{
	uhd::set_thread_priority_safe();
	uhd::time_spec_t refer;

	// add the other argument
	string tx_ant, rx_ant, tx_subdev, rx_subdev, ref, otw;
	double tx_bw, rx_bw, lo_off;
	// USRP setting
	po::options_description desc("Allowed options");
	desc.add_options()("help", "help message")("txip", po::value<string>(&usrp_tx_ip)->default_value("addr=192.168.10.2"), "tx usrp's IP")("rxip", po::value<string>(&usrp_rx_ip)->default_value("addr=192.168.10.4"), "rx usrp's IP")("in", po::value<string>(&in_name)->default_value("wcs_trace/tx_time_signals.bin"), "binary samples file")("out", po::value<string>(&out_name)->default_value("txrx_out.bin"), "signal file")("r", po::value<double>(&rate)->default_value(1e6), "sampling rate")("f", po::value<double>(&freq)->default_value(910e6), "RF center frequency in Hz")("g", po::value<double>(&gain)->default_value(0.0), "gain for the RF chain")("s", po::value<double>(&r_sec)->default_value(RECV_SEC), "recording seconds")("c", po::value<size_t>(&r_cnt)->default_value(90), "round count")("tx-ant", po::value<string>(&tx_ant)->default_value("TX/RX"), "transmit antenna selection")("rx-ant", po::value<string>(&rx_ant)->default_value("TX/RX"), "receive antenna selection")("tx-subdev", po::value<string>(&tx_subdev)->default_value("A:0"), "transmit subdevice specification")("rx-subdev", po::value<string>(&rx_subdev)->default_value("A:0"), "receive subdevice specification")("tx-bw", po::value<double>(&tx_bw), "analog transmit filter bandwidth in Hz")("rx-bw", po::value<double>(&rx_bw), "analog receive filter bandwidth in Hz")("ref", po::value<string>(&ref)->default_value("external"), "clock reference (internal, external, mimo)")("otw", po::value<string>(&otw)->default_value("sc16"), "specify the over-the-wire sample mode")("lo_off", po::value<double>(&lo_off), "Offset for frontend LO in Hz (optional)");

	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	if (vm.count("help"))
	{
		cout << boost::format("UHD TX samples from file %s") % desc << endl;
		return ~0;
	}

	// Init
	init_sys();

	std::signal(SIGINT, &sig_int_handler);
	std::cout << "Press Ctrl + C to stop streaming..." << std::endl;

	// Willy : initial the whole message
	readerInit();
	// Willy : generate the query sequence
	gen_query();
	gen_ACK();
	pkt_rxsack = new gr_complex[SACK.size() * 2];
	if (pkt_rxsack != NULL)
	{
		memset(pkt_rxsack, 0, sizeof(gr_complex) * SACK.size() * 2);
	}

	// load data after filter
	ofstream outfile2;
	outfile2.open("filter_samples_n.bin", std::ofstream::binary);

	// load data sent
	ofstream outf2;
	outf2.open("tx_samples.bin", std::ofstream::binary);

	// load data gate
	ofstream outf3;
	outf3.open("gate.bin", std::ofstream::binary);

	ofstream outf4;
	outf4.open("raw_rx.bin", std::ofstream::binary);

	//cout << "RN16 D: " << RN16_D <<endl; // 575

	// Tx cleaning
	tx_md.start_of_burst = true;
	tx_md.end_of_burst = false;
	tx_md.has_time_spec = false;

	usrp_tx->get_device()->send(zeros, SYM_LEN, tx_md, C_FLOAT32, S_ONE_PKT);

	tx_md.start_of_burst = false;
	tx_md.end_of_burst = true;
	usrp_tx->get_device()->send(zeros, SYM_LEN, tx_md, C_FLOAT32, S_ONE_PKT);

	boost::this_thread::sleep(boost::posix_time::milliseconds(WARM_UP_TIME));
	cout << "Current clock time: " << usrp_tx->get_time_now().get_real_secs() << endl;
	tx_md.time_spec = usrp_tx->get_time_now() + uhd::time_spec_t(1, 0, 1e8 / inter); // set tx timer at (now + 1) sec

	stream_cmd.time_spec = time_start_recv = tx_md.time_spec;
	cout << "Configured Tx time: " << time_start_recv.get_real_secs() << endl;
	cout << "Configured Tx tick: " << time_start_recv.get_tick_count(rate) << endl;
	stream_cmd.stream_now = false;
	usrp_rx->issue_stream_cmd(stream_cmd);

	//set the antenna
	if (vm.count("tx-ant"))
		usrp_tx->set_tx_antenna(tx_ant);
	if (vm.count("rx-ant"))
		usrp_rx->set_rx_antenna(rx_ant);

	//always select the subdevice first, the channel mapping affects the other settings
	if (vm.count("tx-subdev"))
		usrp_tx->set_tx_subdev_spec(tx_subdev);
	if (vm.count("rx-subdev"))
		usrp_rx->set_rx_subdev_spec(rx_subdev);

	uhd::tune_request_t tx_tune_request;
	if (vm.count("lo_off"))
		tx_tune_request = uhd::tune_request_t(freq, lo_off);
	else
		tx_tune_request = uhd::tune_request_t(freq);

	//set the analog frontend filter bandwidth
	if (vm.count("tx-bw"))
	{
		std::cout << boost::format("Setting TX Bandwidth: %f MHz...") % tx_bw << std::endl;
		usrp_tx->set_tx_bandwidth(tx_bw);
		std::cout << boost::format("Actual TX Bandwidth: %f MHz...") % usrp_tx->get_tx_bandwidth() << std::endl
				  << std::endl;
	}
	if (vm.count("rx-bw"))
	{
		std::cout << boost::format("Setting RX Bandwidth: %f MHz...") % (rx_bw / 1e6) << std::endl;
		usrp_rx->set_rx_bandwidth(rx_bw);
		std::cout << boost::format("Actual RX Bandwidth: %f MHz...") % (usrp_rx->get_rx_bandwidth() / 1e6) << std::endl
				  << std::endl;
	}

	// new setting
	std::cout << boost::format("Setting TX Rate: %f Msps...") % (rate / 1e6) << std::endl;
	usrp_tx->set_tx_rate(rate);
	//std::cout << boost::format("Actual TX Rate: %f Msps...") % (tx_usrp->get_tx_rate()/1e6) << std::endl << std::endl;

	size_t tx_round = 0;
	size_t query_round = 0;
	size_t rx_cnt = 0;
	size_t rx_cnt2 = 0;

	float cwAmpl = 0.0;
	bool flag = 0;   // gate done
	bool failed = 0; // gate done
	// correlation
	int n_samples_TAG_BIT = TAG_BIT_D * (s_rate / pow(10, 6)) / 5;

	// Rx cleaning
	size_t done_cleaning;
	done_cleaning = 0;
	while (!done_cleaning)
	{
		usrp_rx->get_device()->recv(pkt_rx, SYM_LEN * 2, rx_md, C_FLOAT32, R_ONE_PKT); // mod to *2
		if (rx_md.time_spec.get_real_secs() >= time_start_recv.get_real_secs())
		{
			done_cleaning = 1;
		}
	}

	// let the first sample use the configured timer
	tx_md.start_of_burst = true;
	tx_md.has_time_spec = true;
	tx_md.end_of_burst = false;
	size_t logall = 1;

	//while(!stop_signal && tx_round < 1) { //&& tx_round < 3
	while (tx_round < 10 && query_round < 100 && rx_cnt < s_cnt)
	{
		size_t tx_samples =  0;
		size_t clean_cnt = 0;
		size_t read_cnt = 0;
		size_t tx_cnt = 0;
		int rn16Index;
		while (gen2_logic_status != IDLE && query_round < 100)
		{
			switch (gen2_logic_status)
			{
			case INIT:
				query_cnt = 0;
				tx_samples = 0;
				flag = 0;
				afterGate.clear();
				beforeGate.clear();
				gen_ACK();				
				if (rx_cnt + SYM_LEN * 2 < s_cnt)
				{
					tx_md.end_of_burst = true;
					stream_cmd.stream_now = false;
					usrp_rx->issue_stream_cmd(stream_cmd);
					usrp_tx->get_device()->send(zeros, SYM_LEN, tx_md, C_FLOAT32, S_ONE_PKT);
				}
				//cout << "Current Rx tick: " << usrp_rx->get_time_now().get_real_secs() << endl;
				//cout << "Current Tx time: " << usrp_tx->get_time_now().get_real_secs() << endl;
				tx_md.time_spec = usrp_tx->get_time_now() + uhd::time_spec_t(1, 0, 1e8 / inter);
				//tx_md.time_spec = usrp_tx->get_time_now();
				stream_cmd.time_spec = time_start_recv = tx_md.time_spec;
				//cout << "Configured Tx time: " << time_start_recv.get_real_secs() << endl;
				//cout << "Configured Tx tick: " << time_start_recv.get_tick_count(rate) << endl;

				done_cleaning = 0;

				while (!done_cleaning)
				{
					usrp_rx->get_device()->recv(pkt_rxtmp, SYM_LEN * 2, rx_md, C_FLOAT32, R_ONE_PKT); // mod to *2
					if (rx_md.time_spec.get_real_secs() >= time_start_recv.get_real_secs())
					{
						done_cleaning = 1;
					}
				}
				tx_md.end_of_burst = false;
				tx_md.start_of_burst = true;
				tx_md.has_time_spec = true;

				done_cleaning = 0;
				clean_cnt = 0;
				while (!done_cleaning)
				{
					tx_samples += usrp_tx->get_device()->send(&Query.front() + tx_samples, SYM_LEN, tx_md, C_FLOAT32, S_ONE_PKT);
					tx_md.start_of_burst = false;
					tx_md.has_time_spec = false;
					usrp_rx->get_device()->recv(pkt_rxtmp, SYM_LEN*2, rx_md, C_FLOAT32, R_ONE_PKT);
					if (abs(pkt_rxtmp[0]) > 0.1)
					{
						done_cleaning = 1;
						cout << "cleaning!!" << endl;						
					}
					else
						clean_cnt++;
				}
				cout << "clean: " << clean_cnt * SYM_LEN << endl;
				for (size_t i; i<clean_cnt; i++)
					usrp_rx->get_device()->recv(pkt_rxtmp, SYM_LEN*2, rx_md, C_FLOAT32, R_ONE_PKT);
				gen2_logic_status = SEND_QUERY;
				break;

			case SEND_QUERY:
				query_round++;
				done_cleaning = 0;
				while (tx_samples < Query.size() && query_cnt < QUERY_RXSIZE) // Q size: 6754
				{
					//cout << "sent: " << tx_samples * 2 << " recved: " << query_cnt << endl;
					if (tx_samples < Query.size() && query_cnt + 2*SYM_LEN >= tx_samples * 2 ) {
						tx_cnt = SYM_LEN;
						if (tx_samples + SYM_LEN > Query.size()) // avoid the empty samples
							tx_cnt = Query.size() - tx_samples;
						tx_samples += usrp_tx->get_device()->send(&Query.front() + tx_samples, tx_cnt, tx_md, C_FLOAT32, S_ONE_PKT);
					}
					
					read_cnt = SYM_LEN * 2;
					if (QUERY_RXSIZE - query_cnt < read_cnt)
						read_cnt = QUERY_RXSIZE - query_cnt;
					read_cnt = usrp_rx->get_device()->recv(query_rx + query_cnt, read_cnt, rx_md, C_FLOAT32, R_ONE_PKT);				
					if (rx_cnt < s_cnt) {
						if (read_cnt > rx_cnt - s_cnt)
							read_cnt = rx_cnt - s_cnt;
						memcpy(pkt_rx + rx_cnt, query_rx + query_cnt, read_cnt * sizeof(gr_complex));
						rx_cnt += read_cnt;
						query_cnt += read_cnt;
					}
					// kate 1115
					// call filter and gate
					if (query_cnt > rn16EndSize && flag == 0)
					{
						filter();
						cwAmpl = gate_impl();
						if (afterGate.size() == 250)
						{
							//cout << "Gate done. " << cwAmpl << endl;
							flag = 1;							
							gen2_logic_status = DECODE_RN16;							
							break;
							// kate 1115: why?
							//if (tx_samples > 6700)
							//	break;
						}
					}
				}

				// log boundary
				/*
				if (rx_cnt  + 20 < s_cnt)
				{					
					memcpy(pkt_rx + rx_cnt, zeros, 20 * sizeof(gr_complex));
					rx_cnt += 20;
				}
				*/

				if (flag == 0) {
					gen2_logic_status = FAIL;
					cout << "Gate failed. " << endl;
				}
				//cout << "======= query cnt: " << query_cnt << " tx_sample " << tx_samples * 2 << " RXSIZE " << rn16EndSize << endl;

				break;

			case DECODE_RN16:
				rn16Index = correlate(n_samples_TAG_BIT, cwAmpl);
				//cout << "rn16index: " << rn16Index << endl;
				// call decoding
				if (rn16Index > 10 || rn16Index == 0)
				{
					fprintf(stderr, "rn16 detection failure\n");
					gen2_logic_status = FAIL;
					break;
				}
				rn16Decode(rn16Index + 60);

				// remove the last bit of RN16
				// RN16_bits.erase(RN16_bits.begin()+RN16_bits.size()-1);
				if (RN16_bits.size() == 16)
					gen2_logic_status = SEND_ACK;
				else
					gen2_logic_status = FAIL;

				break;

			case SEND_ACK:
				tx_samples = 0; // avoid covering the other data
				/**Complete the ACK msg**/						
				for (size_t i = 0; i < RN16_bits.size(); i++)
				{
					if (RN16_bits[i] == 1)
						SACK.insert(SACK.end(), data_1.begin(), data_1.end());
					else
						SACK.insert(SACK.end(), data_0.begin(), data_0.end());
				}
				SACK.insert(SACK.end(), cw_ack.begin(), cw_ack.end()); // 4575 samples

				while (tx_samples < SACK.size())
				{
					tx_cnt = SYM_LEN;

					if (tx_samples + SYM_LEN > SACK.size())
						tx_cnt = SACK.size() - tx_samples;
					tx_samples += usrp_tx->get_device()->send(&SACK.front() + tx_samples, tx_cnt, tx_md, C_FLOAT32, S_ONE_PKT);
					if (rx_cnt < s_cnt)
					{
						read_cnt = tx_cnt * 2;
						if (s_cnt - rx_cnt < read_cnt)
							read_cnt = s_cnt - rx_cnt;
						read_cnt = usrp_rx->get_device()->recv(pkt_rxtmp, read_cnt, rx_md, C_FLOAT32, R_ONE_PKT);
						// storing all received data in pkt_rx for debug
						if (logall)
							memcpy(pkt_rx + rx_cnt, pkt_rxtmp, read_cnt * sizeof(gr_complex));
						else
							memcpy(pkt_rx + rx_cnt2, pkt_rxtmp, read_cnt * sizeof(gr_complex));
						rx_cnt += read_cnt;
						rx_cnt2 += read_cnt;
					}
				}
				gen2_logic_status = IDLE;
				break;
			case FAIL:
				if (rx_cnt >= s_cnt)
				{
					gen2_logic_status = IDLE;
					break;
				}
				gen2_logic_status = INIT;
				break;

			case IDLE:
				break;

			default:
				if (flag == 1)
					gen2_logic_status = DECODE_RN16;
				else
					gen2_logic_status = FAIL;
				break;
			}
		}
		tx_round++;
		cout << "tx round: " << tx_round << ", q round: " << query_round << ", rx count: " << rx_cnt << endl; // 33800 correct
		gen2_logic_status = INIT;
	}
	if (outfile2.is_open())
		outfile2.write((const char *)&beforeGate.front(), beforeGate.size() * sizeof(gr_complex));

	if (outf3.is_open()) // dump the gate data
		outf3.write((const char *)&afterGate.front(), afterGate.size() * sizeof(gr_complex));

	tx_md.start_of_burst = false;
	tx_md.end_of_burst = true;
	usrp_tx->get_device()->send(ones, SYM_LEN, tx_md, C_FLOAT32, S_ONE_PKT);

	if (outfile2.is_open())
		outfile2.close();
	if (outf2.is_open())
		outf2.close();
	if (outf3.is_open())
		outf3.close();
	if (outf4.is_open())
		outf4.close();

	dump_signals();
	end_sys();
	boost::this_thread::sleep(boost::posix_time::seconds(1));
	cout << "Terminate systems ... " << endl;
	return 0;
}
