<?php

	namespace OPNsense\DynamicDNS\Plugins;

	use OPNsense\Core\Backend;

	class GoogleCloudDNS {

		const GCLOUD_MISSING_SERVICE_ACCOUNT_ERROR = 200;
		const GCLOUD_MISSING_ZONE = 201;
		const GCLOUD_INVALID_SERVICE_ACCOUNT = 202;
		const GCLOUD_MISSING_TYPE = 203;

		const GCLOUD_RECORD_TYPES = ['A','AAAA','MX','NS'];


		private $zoneName;
		private $serviceAccount;

		private $env;

		/**
		 * GoogleCloudDNS constructor.
		 *
		 * @param string $serviceAccount
		 * @param string $zoneName
		 */
		public function __construct($serviceAccount,$zoneName) {
			if (!empty($serviceAccount)) {
				# Extract the gcloud project from the key data.
				$this->serviceAccount = json_decode($serviceAccount);
				$project = $this->serviceAccoun->project_id;
				$account = $this->serviceAccoun->client_email;
				if (empty($project)) {
					log_error("Google Cloud DNS: unable to extract project name from Google Cloud DNS JSON key");
					return(1);
				} else {
					log_error("Google Cloud DNS: Google Cloud DNS project name: {$project}");
				}
			} else {
				log_error("Google Cloud DNS: no key for Google Cloud DNS was specified");
				return(1);
			}

			# Preparations for gcloud CLI.
			$config = "gcloud-dns-{$zoneName}-{$project}";
			$keyFile = tempnam(sys_get_temp_dir(), $config);

			file_put_contents($keyFile, $serviceAccount);
			chmod($keyFile, 0600);
			$this->env['CLOUDSDK_ACTIVE_CONFIG_NAME'] = $config;
			$this->env['CLOUDSDK_CORE_PROJECT'] = $project;
			# Ensure that a working gcloud config exists.
			self::runShellCmd("/usr/local/bin/gcloud config configurations create ${config}", $this->env);
			self::runShellCmd("/usr/local/bin/gcloud config configurations activate ${config}", $this->env);
			self::runShellCmd("/usr/local/bin/gcloud auth activate-service-account --key-file=${keyFile}", $this->env);
			self::runShellCmd("/usr/local/bin/gcloud config set account ${account}", $this->env);
			self::runShellCmd("/usr/local/bin/gcloud config set project ${project}", $this->env);

		}

		public function updateRecord ($hostname, $ipAddress, $ttl, $type) {
			if (!$this->startTransaction())
				return $this->abortTransaction();

			if ($records = $this->getRecord($hostname, $type) !== null) {
				foreach ($records as $existing) {
					foreach ($existing->rrdatas as $existingIp) {
						if($ipAddress != $existingIp || $ttl != $existing->ttl)
							$this->removeRecord($existing->name, $existing->type, $existingIp, $existing->ttl);
					}
				}
			};
			$this->addRecord($hostname, $type, $ipAddress, $ttl);
			$this->executeTransaction();
		}



		private function addRecord ($hostname, $type, $ip, $ttl) {
			$results = self::runShellCmd("/usr/local/bin/gcloud dns record-sets transaction add {$ip} --name='{$hostname}' --type='{$type}' --ttl='{$ttl}' --zone='{$this->zoneName}' --format=json",$this->env);
			return $results['code'] == 0;
		}

		private function remove ($hostname, $type, $ip, $ttl) {
			$results = self::runShellCmd("/usr/local/bin/gcloud dns record-sets transaction remove {$ip} --name='{$hostname}' --type='{$type}' --ttl='{$ttl}' --zone='{$this->zoneName}' --format=json",$this->env);
			return $results['code'] == 0;
		}

		private function startTransaction () {
			$results = self::runShellCmd("/usr/local/bin/gcloud dns record-sets transaction start --zone='{$this->zoneName}' --format=json",$this->env);
			return $results['code'] == 0;
		}

		private function abortTransaction () {
			$results = self::runShellCmd("/usr/local/bin/gcloud dns record-sets transaction abort --zone='{$this->zoneName}' --format=json",$this->env);
			return $results['code'] == 0;
		}

		private function executeTransaction () {
			$results = self::runShellCmd("/usr/local/bin/gcloud dns record-sets transaction execute --zone='{$this->zoneName}' --format=json",$this->env);
			return $results['code'] == 0;
		}

		/**
		 * @param string $hostname
		 * @param string $type
		 *
		 * @return array|null

		 */
		public function getRecord ($hostname, $type) {
			$results = self::runShellCmd("/usr/local/bin/gcloud dns record-sets list --zone='{$this->zoneName}' --name='{$hostname}' --type='{$type}' --format=json --limit=1",$this->env);

			if ($results['code'] == 0 && $data = json_decode($results['output']))
				return $data;

			return null;
			/** example return
			[
			  {
				"kind": "dns#resourceRecordSet",
				"name": "opnsense.location.domain.com.",
				"rrdatas": [
				  "192.168.0.1"
			  ],
				"ttl": 300,
				"type": "A"
			  }
			]
			*/
		}

		/**
		 * Check wether the Google Cloud plugin is installed.
		 * @return array status action
		 */
		public function getGcloudPluginStatusAction()
		{
			$result = array("result" => "0");

			$mdlAcme = $this->getModel();

			// Check if the required plugin is installed
			if ((string)$this->isPluginInstalled('google-cloud-sdk') == "1") {
				$result['result'] = "1";
			}

			return $result;
		}
		/**
		 * check if the specfied plugin is installed
		 * @param $name plugin/package name
		 * @return bool is the plugin installed
		 */
		public function isPluginInstalled($name)
		{
			$backend = new Backend();
			return trim($backend->configdRun('firmware plugin ' . escapeshellarg($name)));
		}

		/**
		 * Lifted from the AcmeClient
		 *
		 * @param string $procCmd
		 * @param array $procEnv
		 *
		 * @return array
		 */
		private static function runShellCmd ($procCmd, $procEnv = array())
		{
			$result = [
				'executed'=>false,
				'message' => null,
				'output' => null,
				'code' => null,
			];
			$procDesc = [
				0 => ["pipe", "r"],
				1 => ["pipe", "w"],
				2 => ["pipe", "w"]
			];

			$procPipes = [];
			$proc = proc_open($procCmd, $procDesc, $procPipes, null, $procEnv);

			if (is_resource($proc)) {

				fclose($procPipes[0]);
				$result['output'] =  stream_get_contents($procPipes[1]);

				fclose($procPipes[1]);
				fclose($procPipes[2]);

				$result['code'] = proc_close($proc);
				$result['message'] = sprintf("Google Cloud DNS: The shell command '%s' returned exit code '%d'", $procCmd, $result);

			} else {
				$result['message'] = sprintf("Google Cloud DNS: Unable to prepare shell command '%s'", $procCmd);
			}
			return $result;
		}

	}