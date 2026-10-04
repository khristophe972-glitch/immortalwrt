'use strict';
'require view';
'require form';
'require fs';

/*
 * GL-MT6000 QoS : formulaire de /etc/config/qos-mt6000.
 * Les validations ci-dessous reprennent celles de qos-mt6000.sh, qui reste
 * l'autorite : une valeur refusee ici l'est aussi par le script.
 */

var SCRIPT = '/usr/lib/qos-mt6000/qos-mt6000.sh';

function validateRate(section_id, value) {
	if (value == null || value === '')
		return _('Required');

	if (value === 'unlimited' || /^[0-9]+(\.[0-9]+)?[kKmMgGtT]?bit$/.test(value))
		return true;

	return _('Use a rate with a unit, for example 850Mbit, or "unlimited".');
}

function validateRtt(section_id, value) {
	if (value == null || value === '')
		return _('Required');

	if (/^(datacentre|lan|metro|regional|internet|oceanic|satellite|interplanetary)$/.test(value) ||
	    /^[0-9]+(us|ms|s)$/.test(value))
		return true;

	return _('Use a duration such as 30ms, or a keyword such as regional.');
}

function validateRange(min, max, label) {
	return function(section_id, value) {
		if (value == null || value === '')
			return true;

		if (!/^-?[0-9]+$/.test(value) || +value < min || +value > max)
			return _('%s must be an integer from %d to %d.').format(label, min, max);

		return true;
	};
}

function validateMemlimit(section_id, value) {
	if (value == null || value === '')
		return true;

	return /^[0-9]+([kKmMgG][bB]?)?$/.test(value) ? true : _('Use a size such as 32Mb.');
}

return view.extend({
	load: function() {
		return L.resolveDefault(fs.exec_direct(SCRIPT, [ 'status' ]), '');
	},

	render: function(status) {
		var m, s, o;

		m = new form.Map('qos-mt6000', _('GL-MT6000 QoS'),
			_('Root cake_mq qdisc of the WAN port. Saving applies the configuration: the script validates ' +
			  'every value, replaces the root qdisc (tc qdisc replace) and writes the cake_mq tuning. ' +
			  'It is also applied again each time the trigger interface comes up.'));

		s = m.section(form.NamedSection, 'global', 'global');
		s.addremove = false;
		s.tab('qdisc', _('Root qdisc'));
		s.tab('tuning', _('cake_mq tuning'));

		o = s.taboption('qdisc', form.Flag, 'enabled', _('Enable'),
			_('When off, the root qdisc is left untouched.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('qdisc', form.Value, 'device', _('Device'),
			_('Network device that carries the qdisc (the WAN port).'));
		o.datatype = 'netdevname';
		o.placeholder = 'eth1';
		o.rmempty = false;

		o = s.taboption('qdisc', form.Value, 'trigger_iface', _('Trigger interface'),
			_('The configuration is applied again when this interface comes up.'));
		o.datatype = 'uciname';
		o.placeholder = 'wan';

		o = s.taboption('qdisc', form.ListValue, 'qdisc', _('Qdisc'));
		o.value('cake_mq', _('cake_mq (one instance per queue)'));
		o.value('cake', _('cake (single instance)'));
		o.default = 'cake_mq';

		o = s.taboption('qdisc', form.Value, 'bandwidth', _('Bandwidth'),
			_('Shaper rate with its unit, for example 850Mbit, or "unlimited".'));
		o.placeholder = '850Mbit';
		o.rmempty = false;
		o.validate = validateRate;

		o = s.taboption('qdisc', form.ListValue, 'diffserv', _('Priority tins'));
		o.value('besteffort', _('besteffort (1 tin)'));
		o.value('precedence', _('precedence'));
		o.value('diffserv3', _('diffserv3 (3 tins)'));
		o.value('diffserv4', _('diffserv4 (4 tins)'));
		o.value('diffserv8', _('diffserv8 (8 tins)'));
		o.default = 'diffserv4';

		o = s.taboption('qdisc', form.ListValue, 'flow_isolation', _('Flow isolation'));
		o.value('triple-isolate', 'triple-isolate');
		o.value('dual-srchost', 'dual-srchost');
		o.value('dual-dsthost', 'dual-dsthost');
		o.value('hosts', 'hosts');
		o.value('srchost', 'srchost');
		o.value('dsthost', 'dsthost');
		o.value('flows', 'flows');
		o.value('flowblind', 'flowblind');
		o.default = 'triple-isolate';

		o = s.taboption('qdisc', form.Flag, 'nat', _('NAT lookup'),
			_('Look up the pre-NAT addresses in conntrack for flow isolation.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('qdisc', form.Flag, 'wash', _('Wash DSCP'),
			_('Clear the DSCP field of outgoing packets after classification.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('qdisc', form.ListValue, 'ack_filter', _('ACK filter'));
		o.value('none', _('Off'));
		o.value('ack-filter', 'ack-filter');
		o.value('ack-filter-aggressive', 'ack-filter-aggressive');
		o.default = 'ack-filter';

		o = s.taboption('qdisc', form.Flag, 'split_gso', _('Split GSO'),
			_('Split GSO super-packets so that each flow is shaped by real packets.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('qdisc', form.Value, 'rtt', _('RTT'),
			_('Round-trip time that the AQM assumes: a duration such as 30ms, or a keyword ' +
			  '(metro 10ms, regional 30ms, internet 100ms). It should cover the RTT of the flows ' +
			  'that must keep their full throughput.'));
		o.placeholder = '30ms';
		o.rmempty = false;
		o.validate = validateRtt;

		o = s.taboption('qdisc', form.ListValue, 'link_type', _('Link layer'));
		o.value('noatm', _('Ethernet (noatm)'));
		o.value('ptm', _('PTM (VDSL2)'));
		o.value('atm', _('ATM'));
		o.default = 'noatm';

		o = s.taboption('qdisc', form.Value, 'overhead', _('Overhead (bytes)'),
			_('Per-packet overhead added by the link layer.'));
		o.placeholder = '38';
		o.rmempty = false;
		o.validate = validateRange(-64, 256, _('Overhead'));

		o = s.taboption('qdisc', form.Value, 'mpu', _('Minimum packet unit (bytes)'));
		o.placeholder = '84';
		o.rmempty = false;
		o.validate = validateRange(0, 256, _('MPU'));

		o = s.taboption('qdisc', form.Value, 'memlimit', _('Memory limit'),
			_('Optional, for example 32Mb. Left empty, cake chooses.'));
		o.optional = true;
		o.validate = validateMemlimit;

		o = s.taboption('tuning', form.Value, 'sync_time_ns', _('Sync time (ns)'),
			_('Interval between two synchronizations of the cake_mq instances.'));
		o.datatype = 'uinteger';
		o.optional = true;

		o = s.taboption('tuning', form.Value, 'active_window_ns', _('Activity window (ns)'),
			_('How long an instance still counts as active after its last packet.'));
		o.datatype = 'uinteger';
		o.optional = true;

		o = s.taboption('tuning', form.Value, 'active_release', _('Activity release'),
			_('Hysteresis of the activity count. Left empty, the kernel value is kept.'));
		o.datatype = 'uinteger';
		o.optional = true;

		o = s.taboption('tuning', form.ListValue, 'tin_share', _('Per-tin share'),
			_('Divide each tin threshold by the instances that recently served that tin.'));
		o.value('0', _('Off'));
		o.value('1', _('On'));
		o.default = '0';
		o.optional = true;

		o = s.taboption('tuning', form.Value, 'tin_hold', _('Tin hold (sync periods)'),
			_('How many sync periods a tin stays active on an instance after its last packet.'));
		o.datatype = 'min(1)';
		o.placeholder = '8';
		o.optional = true;

		o = s.taboption('tuning', form.Value, 'tin_cap', _('Tin cap (%)'),
			_('With per-tin share, a limited tin never exceeds this share of its instance rate. 0 disables.'));
		o.datatype = 'range(0,100)';
		o.placeholder = '0';
		o.optional = true;

		return m.render().then(function(node) {
			return E('div', {}, [
				node,
				E('h3', {}, _('Status')),
				E('pre', { 'style': 'overflow:auto' }, (status && status.trim()) || _('No status available.'))
			]);
		});
	}
});
